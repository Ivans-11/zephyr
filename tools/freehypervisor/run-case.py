#!/usr/bin/env python3
"""Build and run declarative Zephyr-FHV regression cases."""
from __future__ import annotations

import argparse
from pathlib import Path
import os
import subprocess
import sys

from build import Builder, HERE, SAMPLE, build_lock, load, run


def run_guest(builder: Builder, output: Path, case: dict, timeout: int, cpu: str, log: Path):
    run(["cmake", "--build", output, "--target", "qemu_kernel_target"], env=builder.env)
    images = output / "zephyr"
    locore = images / "zephyr-qemu-locore.elf"
    main = images / "zephyr-qemu-main.elf"
    for image in (locore, main):
        if not image.is_file():
            raise ValueError(f"missing QEMU image: {image}")
    command = [
        os.environ.get("FH_QEMU", "qemu-system-x86_64"),
        "-machine", "q35,acpi=off", "-accel", "kvm", "-cpu", cpu,
        "-smp", str(builder.profile["cpus"]), "-m", builder.profile["memory"],
        "-nographic", "-no-reboot", "-net", "none",
        "-device", f"loader,file={main}", "-kernel", str(locore),
    ]
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"QEMU log: {log}", flush=True)
    with log.open("w") as stream:
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"QEMU timed out after {timeout}s; log: {log}") from error
    text = log.read_text(errors="replace")
    print(text, end="")
    missing = [marker for marker in case["success_markers"] if marker not in text]
    failures = [marker for marker in builder.profile["fail_markers"] if marker in text]
    if result.returncode != 0 or missing or failures:
        raise RuntimeError(f"FAIL: QEMU exit={result.returncode}, missing={missing}, "
                           f"failures={failures}; log: {log}")


def main():
    cases = load(HERE / "tests/cases.toml")
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--case", choices=cases)
    selection.add_argument("--all", action="store_true")
    selection.add_argument("--vm-config", type=Path, action="append")
    parser.add_argument("--expect", action="append", default=[],
                        help="additional guest success marker (custom guest only)")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--timeout", type=int)
    parser.add_argument("--cpu", help="QEMU CPU model")
    args = parser.parse_args()
    if args.list:
        for name, case in cases.items():
            print(f"{name}: {case['description']}")
        return
    if args.timeout is not None and args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.expect and not args.vm_config:
        parser.error("--expect requires --vm-config")
    if args.vm_config:
        cases = {"custom": {
            "conf": "linux.conf", "overlay": "linux.overlay",
            "success_markers": ["FH_RESULT core-static result=PASS", *args.expect],
        }}
        selected = ["custom"]
    else:
        selected = list(cases) if args.all else [args.case or "linux"]
    with build_lock():
        builder = Builder()
        for name in selected:
            case = cases[name]
            for key in ("conf", "overlay"):
                if key in case and not (SAMPLE / case[key]).is_file():
                    raise ValueError(f"{name}: missing {key}: {case[key]}")
            configs = [p.resolve() for p in args.vm_config or []]
            if case.get("guest") == "linux":
                configs = [builder.linux_guest()]
            output = builder.host(name, case, configs)
            if args.build_only:
                print(f"Built {name}: {output}")
                continue
            run_guest(builder, output, case,
                      args.timeout or builder.profile["timeout"],
                      args.cpu or os.environ.get("FH_QEMU_CPU", builder.profile["cpu"]),
                      builder.work / "artifacts" / f"{name}.log")
            print(f"PASS: {name}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
