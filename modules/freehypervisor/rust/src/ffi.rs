//! Private C bridge to the Zephyr adapter; the core only uses axvisor-api.
use core::ffi::c_void;

unsafe extern "C" {
    pub(crate) fn fh_console_write(bytes: *const u8, length: usize);
    pub(crate) fn fh_console_read(bytes: *mut u8, length: usize) -> usize;
    pub(crate) fn fh_console_acquire();
    pub(crate) fn fh_console_release();
    pub(crate) fn fh_rust_alloc(size: usize, align: usize) -> *mut u8;
    pub(crate) fn fh_rust_dealloc(ptr: *mut u8, size: usize, align: usize);
    pub(crate) fn fh_rust_abort() -> !;

    pub(crate) fn fh_host_cpu_count() -> usize;
    pub(crate) fn fh_current_cpu_id() -> usize;
    pub(crate) fn fh_host_tsc_frequency_mhz() -> u32;

    #[cfg(target_arch = "x86_64")]
    pub(crate) fn fh_prepare_x86_virtualization();

    pub(crate) fn fh_alloc_frames(count: usize, align_frames: usize) -> u64;
    pub(crate) fn fh_free_frames(paddr: u64, count: usize);
    pub(crate) fn fh_phys_to_virt(paddr: u64) -> *mut u8;
    pub(crate) fn fh_virt_to_phys(vaddr: *const u8) -> u64;

    pub(crate) fn fh_task_spawn(
        name: *const u8,
        name_len: usize,
        stack_size: usize,
        cpu_mask: usize,
        entry: extern "C" fn(*mut c_void),
        context: *mut c_void,
    ) -> usize;
    pub(crate) fn fh_task_join(handle: usize);
    pub(crate) fn fh_task_current() -> usize;
    pub(crate) fn fh_task_yield();

    pub(crate) fn fh_wait_queue_create() -> usize;
    pub(crate) fn fh_wait_queue_destroy(queue: usize);
    pub(crate) fn fh_wait_queue_wait(queue: usize);
    pub(crate) fn fh_wait_queue_wait_until(
        queue: usize,
        predicate: extern "C" fn(*mut c_void) -> bool,
        context: *mut c_void,
    );
    pub(crate) fn fh_wait_queue_wake_one(queue: usize);
    pub(crate) fn fh_wait_queue_wake_all(queue: usize);

    pub(crate) fn fh_monotonic_nanos() -> u64;
    pub(crate) fn fh_set_oneshot_timer(deadline_ns: u64);
    pub(crate) fn fh_irq_handle(vector: usize) -> bool;
    pub(crate) fn fh_irq_register(vector: usize, handler: extern "C" fn(usize)) -> bool;

    pub(crate) static _percpu_load_start: u8;
    pub(crate) static _percpu_load_end: u8;
}
