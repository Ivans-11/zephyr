#!/usr/bin/env python3
"""Build guest assets, the Rust adapter, and the Zephyr host."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SAMPLE = ROOT / "samples/freehypervisor"
RUST = ROOT / "modules/freehypervisor/rust"


def load(path: Path) -> dict:
    with path.open("rb") as stream:
        return tomllib.load(stream)


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


@contextmanager
def build_lock():
    # All builds share a Cargo lockfile, including builds with different outputs.
    (ROOT / ".work").mkdir(exist_ok=True)
    with (ROOT / ".work/freehypervisor-run.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


class Builder:
    def __init__(self):
        self.work = Path(os.environ.get("FH_WORK_ROOT", ROOT / ".work/freehypervisor")).resolve()
        self.jobs = str(os.cpu_count() or 1)
        self.env = os.environ.copy()
        self.env["ZEPHYR_BASE"] = str(ROOT)
        self.env.setdefault("ZEPHYR_TOOLCHAIN_VARIANT", "host")
        self.profile = load(HERE / "tests/host.toml")
        self.work.mkdir(parents=True, exist_ok=True)

    def linux_source(self) -> Path:
        if override := os.environ.get("FH_LINUX_SOURCE"):
            source = Path(override).resolve()
            if not (source / "Makefile").is_file():
                raise ValueError(f"invalid FH_LINUX_SOURCE: {source}")
            return source
        spec = load(HERE / "tests/assets.toml")["linux"]
        source = self.work / "deps/linux"
        if not (source / ".git").exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            run(["git", "init", source])
        head = subprocess.run(["git", "-C", str(source), "rev-parse", "--verify", "HEAD"],
                              capture_output=True, text=True)
        if head.returncode:
            run(["git", "-C", source, "fetch", "--depth=1", spec["url"], spec["revision"]])
            run(["git", "-C", source, "checkout", "--detach", "FETCH_HEAD"])
        else:
            dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True)
            if head.stdout.strip() != spec["revision"] or dirty:
                raise ValueError(f"managed Linux source changed: {source}; use FH_LINUX_SOURCE explicitly")
        return source

    def linux_guest(self) -> Path:
        source = self.linux_source()
        output = self.work / "build/linux-guest"
        assets = self.work / "build/guests"
        output.mkdir(parents=True, exist_ok=True)
        assets.mkdir(parents=True, exist_ok=True)
        env = self.env | {"KBUILD_BUILD_USER": "freehypervisor", "KBUILD_BUILD_HOST": "zephyr-vmx",
                          "KBUILD_BUILD_TIMESTAMP": "1970-01-01 00:00:00 UTC"}
        make = ["make", "-C", source, f"O={output}", "ARCH=x86_64"]
        run([*make, f"KCONFIG_ALLCONFIG={SAMPLE}/guest/linux.config", "allnoconfig"], env=env)
        run([*make, f"-j{os.environ.get('FH_LINUX_JOBS', self.jobs)}", "bzImage"], env=env)
        run(["x86_64-linux-gnu-gcc", "-Os", "-nostdlib", "-static", "-ffreestanding",
             "-fno-pie", "-no-pie", "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
             "-Wl,--build-id=none", "-Wl,-e,_start", SAMPLE / "guest/linux-init.c", "-o", assets / "init"])
        shutil.copy2(output / "arch/x86/boot/bzImage", assets / "bzImage")
        for name in ("initramfs.list", "vm-linux.toml"):
            shutil.copy2(SAMPLE / "guest" / name, assets / name)
        with (assets / "initramfs.cpio").open("wb") as stream:
            run([output / "usr/gen_init_cpio", "initramfs.list"], cwd=assets, stdout=stream)
        return assets / "vm-linux.toml"

    def rust(self, configs: list[Path]) -> Path:
        for path in configs:
            if not path.is_file():
                raise ValueError(f"VM configuration not found: {path}")
        if not configs:
            raise ValueError("Rust core build requires at least one VM configuration")
        env = self.env | {"AXVISOR_VM_CONFIGS": ":".join(str(p.resolve()) for p in configs),
                          "CARGO_TARGET_DIR": str(self.work / "build/rust")}
        options = ["--locked"]
        if (RUST / ".cargo/config.local.toml").is_file():
            options = ["--config", ".cargo/config.local.toml"]
        run(["cargo", f"+{os.environ.get('FH_RUST_TOOLCHAIN', 'nightly-2026-05-28')}", "build",
             *options, "--target", "x86_64-unknown-none", "--release"], cwd=RUST, env=env)
        return self.work / "build/rust/x86_64-unknown-none/release/libfreehypervisor_zephyr.a"

    def host(self, name: str, case: dict, configs: list[Path]) -> Path:
        rust_lib = self.rust(configs) if configs else None
        output = self.work / "build" / f"zephyr-{name}"
        args = ["cmake", "-S", SAMPLE, "-B", output, "-GNinja",
                f"-DBOARD={self.profile['board']}", f"-DZEPHYR_MODULES={ROOT}/modules/freehypervisor",
                f"-DEXTRA_CONF_FILE={SAMPLE / case['conf']}", "-DUSE_CCACHE=0",
                "-DEXTRA_CFLAGS=-U__linux__", "-DCMAKE_C_COMPILER_LAUNCHER=",
                "-DCMAKE_CXX_COMPILER_LAUNCHER="]
        if overlay := case.get("overlay"):
            args.append(f"-DDTC_OVERLAY_FILE={SAMPLE / overlay}")
        if rust_lib:
            args.append(f"-DFREEHYPERVISOR_RUST_LIB={rust_lib}")
        run(args, env=self.env)
        run(["cmake", "--build", output, "-j", os.environ.get("FH_ZEPHYR_JOBS", self.jobs)], env=self.env)
        return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=["guest", "rust"])
    parser.add_argument("--vm-config", type=Path, action="append", default=[])
    args = parser.parse_args()
    if args.target == "guest" and args.vm_config:
        parser.error("--vm-config is only used by the rust target")
    with build_lock():
        builder = Builder()
        if args.target == "guest":
            result = builder.linux_guest()
        else:
            result = builder.rust(args.vm_config or [builder.linux_guest()])
        print(result)


if __name__ == "__main__":
    main()
