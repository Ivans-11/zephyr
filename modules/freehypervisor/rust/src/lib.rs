#![no_std]
#![feature(alloc_error_handler)]

extern crate alloc;

use alloc::alloc::{GlobalAlloc, Layout};
use alloc::boxed::Box;
use alloc::sync::Arc;
use core::ffi::c_void;
use core::fmt::{self, Write};
use core::panic::PanicInfo;
use core::ptr;
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

use axvisor_api::{api_impl, arch, console, host, irq, memory, sync, task, time};

mod ffi;
mod percpu;
use ffi::*;
use percpu::init_percpu_areas;

struct ZephyrAllocator;

unsafe impl GlobalAlloc for ZephyrAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { fh_rust_alloc(layout.size(), layout.align()) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { fh_rust_dealloc(ptr, layout.size(), layout.align()) }
    }
}

#[global_allocator]
static ALLOCATOR: ZephyrAllocator = ZephyrAllocator;

#[alloc_error_handler]
fn alloc_error(layout: Layout) -> ! {
    let mut output = Console;
    let _ = writeln!(output, "FH: Rust allocation failure: {layout:?}");
    unsafe { fh_rust_abort() }
}

#[panic_handler]
fn panic(info: &PanicInfo<'_>) -> ! {
    let mut output = Console;
    let _ = writeln!(output, "FH: Rust panic: {info}");
    unsafe { fh_rust_abort() }
}

struct Console;

impl Write for Console {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        unsafe { fh_console_write(value.as_ptr(), value.len()) };
        Ok(())
    }
}

struct ZephyrLogger;

impl log::Log for ZephyrLogger {
    fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
        metadata.level() <= log::Level::Warn
    }

    fn log(&self, record: &log::Record<'_>) {
        if self.enabled(record.metadata()) {
            unsafe { fh_console_acquire() };
            let mut output = Console;
            let _ = writeln!(output, "FH-CORE [{:5}] {}", record.level(), record.args());
            unsafe { fh_console_release() };
        }
    }

    fn flush(&self) {}
}

static LOGGER: ZephyrLogger = ZephyrLogger;

struct HostIfImpl;

#[api_impl]
impl host::HostIf for HostIfImpl {
    fn get_host_cpu_num() -> usize {
        unsafe { fh_host_cpu_count() }
    }

    fn init_percpu() {
        let cpu = unsafe { fh_current_cpu_id() };
        #[cfg(target_arch = "x86_64")]
        unsafe {
            // Zephyr AP startup does not guarantee VMX-fixed CR0/CR4 bits.
            fh_prepare_x86_virtualization();
        }
        ax_percpu::init_percpu_reg(cpu);
    }
}

struct ConsoleIfImpl;

#[api_impl]
impl console::ConsoleIf for ConsoleIfImpl {
    fn write_bytes(bytes: &[u8]) {
        unsafe { fh_console_write(bytes.as_ptr(), bytes.len()) }
    }

    fn read_bytes(bytes: &mut [u8]) -> usize {
        unsafe { fh_console_read(bytes.as_mut_ptr(), bytes.len()) }
    }
}

struct MemoryIfImpl;

#[api_impl]
impl memory::MemoryIf for MemoryIfImpl {
    fn alloc_frame() -> Option<memory::PhysAddr> {
        let pa = unsafe { fh_alloc_frames(1, 1) } as usize;
        (pa != 0).then(|| memory::PhysAddr::from_usize(pa))
    }

    fn alloc_contiguous_frames(num_frames: usize, frame_align: usize) -> Option<memory::PhysAddr> {
        let pa = unsafe { fh_alloc_frames(num_frames, frame_align) } as usize;
        (pa != 0).then(|| memory::PhysAddr::from_usize(pa))
    }

    fn dealloc_frame(addr: memory::PhysAddr) {
        unsafe { fh_free_frames(addr.as_usize() as u64, 1) }
    }

    fn dealloc_contiguous_frames(first_addr: memory::PhysAddr, num_frames: usize) {
        unsafe { fh_free_frames(first_addr.as_usize() as u64, num_frames) }
    }

    fn phys_to_virt(addr: memory::PhysAddr) -> memory::VirtAddr {
        memory::VirtAddr::from_usize(unsafe { fh_phys_to_virt(addr.as_usize() as u64) } as usize)
    }

    fn virt_to_phys(addr: memory::VirtAddr) -> memory::PhysAddr {
        memory::PhysAddr::from_usize(
            unsafe { fh_virt_to_phys(addr.as_usize() as *const u8) } as usize
        )
    }
}

struct TaskClosure(Option<Box<dyn FnOnce() + Send + 'static>>);

extern "C" fn task_trampoline(context: *mut c_void) {
    let mut closure = unsafe { Box::from_raw(context.cast::<TaskClosure>()) };
    if let Some(entry) = closure.0.take() {
        entry();
    }
}

struct TaskIfImpl;

#[api_impl]
impl task::TaskIf for TaskIfImpl {
    fn spawn_task_raw(
        options: task::TaskOptions,
        entry: Box<dyn FnOnce() + Send + 'static>,
    ) -> task::TaskHandle {
        let registered = Arc::new(AtomicBool::new(false));
        let registered_for_task = registered.clone();
        let entry = Box::new(move || {
            while !registered_for_task.load(Ordering::Acquire) {
                unsafe { fh_task_yield() };
            }
            entry();
        });
        let context = Box::into_raw(Box::new(TaskClosure(Some(entry)))).cast::<c_void>();
        let cpu_mask = options.cpu_set.unwrap_or(0);
        let handle = unsafe {
            fh_task_spawn(
                options.name.as_ptr(),
                options.name.len(),
                options.stack_size,
                cpu_mask,
                task_trampoline,
                context,
            )
        };
        if handle == 0 {
            unsafe { drop(Box::from_raw(context.cast::<TaskClosure>())) };
            panic!("Zephyr failed to create host task");
        }
        registered.store(true, Ordering::Release);
        task::TaskHandle::from_raw(handle)
    }

    fn join_task(task: task::TaskHandle) {
        unsafe { fh_task_join(task.as_raw()) }
    }

    fn current_task() -> Option<task::TaskHandle> {
        let handle = unsafe { fh_task_current() };
        (handle != 0).then(|| task::TaskHandle::from_raw(handle))
    }

    fn yield_now() {
        unsafe { fh_task_yield() }
    }
}

struct Predicate(Box<dyn Fn() -> bool + Send + 'static>);

extern "C" fn predicate_trampoline(context: *mut c_void) -> bool {
    let predicate = unsafe { &*context.cast::<Predicate>() };
    (predicate.0)()
}

struct SyncIfImpl;

#[api_impl]
impl sync::SyncIf for SyncIfImpl {
    fn create_wait_queue() -> usize {
        unsafe { fh_wait_queue_create() }
    }

    fn destroy_wait_queue(queue: usize) {
        unsafe { fh_wait_queue_destroy(queue) }
    }

    fn wait_queue_wait(queue: usize) {
        unsafe { fh_wait_queue_wait(queue) }
    }

    fn wait_queue_wait_until(queue: usize, condition: Box<dyn Fn() -> bool + Send + 'static>) {
        let mut predicate = Predicate(condition);
        unsafe {
            fh_wait_queue_wait_until(
                queue,
                predicate_trampoline,
                ptr::from_mut(&mut predicate).cast::<c_void>(),
            )
        }
    }

    fn wait_queue_wake_one(queue: usize) {
        unsafe { fh_wait_queue_wake_one(queue) }
    }

    fn wait_queue_wake_all(queue: usize) {
        unsafe { fh_wait_queue_wake_all(queue) }
    }
}

struct TimeIfImpl;

#[api_impl]
impl time::TimeIf for TimeIfImpl {
    fn current_time_nanos() -> time::Nanos {
        unsafe { fh_monotonic_nanos() }
    }

    fn set_oneshot_timer(deadline: time::TimeValue) {
        unsafe { fh_set_oneshot_timer(deadline.as_nanos() as u64) }
    }
}

static IRQ_HANDLERS: [AtomicUsize; 256] = [const { AtomicUsize::new(0) }; 256];

extern "C" fn irq_trampoline(vector: usize) {
    let raw = IRQ_HANDLERS
        .get(vector)
        .map(|slot| slot.load(Ordering::Acquire))
        .unwrap_or(0);
    if raw != 0 {
        let handler: irq::IrqHandler = unsafe { core::mem::transmute(raw) };
        handler(vector);
    }
}

struct IrqIfImpl;

#[api_impl]
impl irq::IrqIf for IrqIfImpl {
    fn handle_irq(vector: usize) -> bool {
        unsafe { fh_irq_handle(vector) }
    }

    fn register_irq_handler(vector: usize, handler: irq::IrqHandler) -> bool {
        let Some(slot) = IRQ_HANDLERS.get(vector) else {
            return false;
        };
        if slot
            .compare_exchange(0, handler as usize, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return false;
        }
        if unsafe { fh_irq_register(vector, irq_trampoline) } {
            true
        } else {
            slot.store(0, Ordering::Release);
            false
        }
    }
}

struct ArchIfImpl;

#[api_impl]
impl arch::ArchIf for ArchIfImpl {
    fn host_tsc_frequency_mhz() -> Option<u32> {
        let mhz = unsafe { fh_host_tsc_frequency_mhz() };
        (mhz != 0).then_some(mhz)
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn fh_rust_init() -> i32 {
    if init_percpu_areas().is_err() {
        return -1;
    }
    let _ = log::set_logger(&LOGGER);
    log::set_max_level(log::LevelFilter::Warn);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn fh_rust_run() {
    axvisor_core::boot::run_static_mode();
}

#[unsafe(no_mangle)]
pub extern "C" fn fh_rust_timer_expired() {
    axvisor_core::vmm::timer::check_events();
}
