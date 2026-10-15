# Zephyr-FHV tests

The tools use one runner, one build module and declarative test configurations:

```text
run-case.py             Select cases, build, run QEMU and check results
build.py                Build guest assets, Rust adapter and Zephyr host
tests/
  cases.toml            Case descriptions, sample configs and success markers
  host.toml             Board, QEMU topology, timeout and failure markers
  assets.toml           Guest Linux source URL and fixed revision
```

The validation application and its guest source files live in
`samples/freehypervisor/`. Generated files stay in `.work/freehypervisor/`;
override this with `FH_WORK_ROOT`.

## Requirements

Use x86_64 Linux with Intel VMX/EPT and access to /dev/kvm. A virtual host also
needs nested VMX enabled. Standard QEMU is sufficient.

Install Zephyr's host dependencies (CMake, Ninja, GCC/G++, Python 3.11 or newer,
dtc, gperf), Linux build dependencies (make, flex, bison, bc, libelf/OpenSSL
development headers), Git, x86_64-linux-gnu-gcc and QEMU x86. In a Python virtual
environment, install `scripts/requirements-base.txt`.

```sh
rustup toolchain install nightly-2026-05-28 --profile minimal
rustup target add --toolchain nightly-2026-05-28 x86_64-unknown-none
```

Cargo.toml selects the tgoskits axvisor-core branch; Cargo.lock pins the commit.
Local development uses the ignored
`modules/freehypervisor/rust/.cargo/config.local.toml` with Cargo patch entries.
The builder loads it automatically. Without that file it uses --locked.
Local patch builds can update Cargo.lock; keep the Git resolution in the
submitted lockfile.

The guest kernel defaults to the Linux revision in tests/assets.toml, fetched
into the work directory. FH_LINUX_SOURCE explicitly selects a different source
tree. No sibling checkout or prebuilt guest is required by default.

## Run cases

```sh
python3 tools/freehypervisor/run-case.py --list
python3 tools/freehypervisor/run-case.py --case contract
python3 tools/freehypervisor/run-case.py --case linux
python3 tools/freehypervisor/run-case.py --all
python3 tools/freehypervisor/run-case.py --case linux --build-only
```

| Case | Checks |
| --- | --- |
| contract | Memory ownership/alignment, task affinity/join, IRQ dispatch, wait/wake ordering, queue destruction and absolute timers |
| lifetime | Adapter contracts plus a deliberately broken lost-wakeup negative control |
| probe | VMX availability |
| linux | Linux userspace startup, PID 1 PASS, guest shutdown and core return |

The default host has two CPUs and 128 MiB RAM. The Linux guest has one vCPU
pinned to host CPU 1 and 64 MiB RAM. These are functional tests.

Logs are stored at `.work/freehypervisor/artifacts/<case>.log`. A case passes
only when QEMU exits with code 0, all declared success markers appear, and no
failure marker appears. Timeouts fail and terminate QEMU. Use --timeout SECONDS
to override the default limit.

## Custom guest and standalone builds

```sh
python3 tools/freehypervisor/run-case.py \
  --vm-config /path/to/vm.toml --expect 'MY_GUEST PASS'
python3 tools/freehypervisor/build.py guest
python3 tools/freehypervisor/build.py rust --vm-config /path/to/vm.toml
```

Repeat --vm-config for multiple VMs, and --expect for multiple required markers.
Custom guests reuse the Linux sample host configuration and report core return
in addition to any --expect markers. Guest image paths resolve relative to each
VM TOML and are embedded into the Rust library. --cpu overrides the QEMU model.

build.py exposes guest/Rust-only builds for packaging; host-only builds use
run-case.py --build-only. All command-line build entry points share a lock.
Zephyr uses host GCC, and Rust uses release opt-level 2 with thin LTO.

The sample's core.conf and kick.conf are low-level modes requiring appropriate
guest configurations; they are not currently standalone regression cases.
