FreeHypervisor adapter validation
=================================

This application exercises the FreeHypervisor Zephyr host adapter in
contract, lifetime, probe, static-Core and IPI-kick modes. The adapter itself
is provided by ``modules/freehypervisor``; the Rust Core archive and Guest
images are prepared by the repository's build scripts.

The application is configured with the module explicitly, for example::

   ZEPHYR_BASE=$PWD west build -b qemu_x86_64 \
     -d build/freehypervisor samples/freehypervisor -- \
     -DZEPHYR_MODULES=$PWD/modules/freehypervisor \
     -DEXTRA_CONF_FILE=$PWD/samples/freehypervisor/contract.conf

The mode-specific configuration files are overlays on top of ``prj.conf``.
The Linux Guest configuration and PID 1 are in ``guest/``. Dependency versions,
source preparation and QEMU execution are in ``tools/freehypervisor``.

To run the Linux Guest on an x86 VMX host, use the repository runner::

   python3 tools/freehypervisor/run-case.py --case linux

The runner builds the pinned Guest kernel and initramfs, embeds them through
the Core's ``AXVISOR_VM_CONFIGS`` build input, links the Rust Core, and boots
the resulting Zephyr Host with QEMU/KVM. Build inputs can be overridden with
local Cargo patches and ``FH_LINUX_SOURCE``; all generated files are kept
under ``.work/freehypervisor``.

The same entry point supports ``contract``, ``lifetime`` and ``probe``. See
``tools/freehypervisor/README.md`` for requirements and success criteria.
