# FreeHypervisor Zephyr module

This in-tree module contains the Zephyr host adapter for the reusable
FreeHypervisor Core. It preserves the adapter's C ABI and Rust
`axvisor_api` implementation from the standalone Zephyr port while making
the source part of this Zephyr fork.

The module is selected explicitly by the application build:

```sh
cmake -DZEPHYR_MODULES=$ZEPHYR_BASE/modules/freehypervisor ...
```

When `CONFIG_FREEHYPERVISOR_CORE=y`, the application must provide the
prepared Rust static library through `FREEHYPERVISOR_RUST_LIB`. The Core
source is a versioned dependency; this module
only supplies the Zephyr host implementation and its build integration.

The reusable validation application is in `samples/freehypervisor`.

The sample owns the validation-mode Kconfig. This module only defines adapter
options. Rust `ffi.rs` declares the private C bridge, `percpu.rs` owns per-CPU
template allocation, and `lib.rs` implements axvisor-api and the Rust runtime.
See `tools/freehypervisor/README.md` for standalone build and regression commands.
