//! Per-CPU template allocation without changing Zephyr's GS base.
use super::{ALLOCATOR, Console};
use crate::ffi::*;
use alloc::alloc::{GlobalAlloc, Layout};
use core::fmt::Write;
use core::ptr;
use core::sync::atomic::{AtomicUsize, Ordering};

static PERCPU_BASE: AtomicUsize = AtomicUsize::new(0);
static PERCPU_STRIDE: AtomicUsize = AtomicUsize::new(0);

fn align_up(value: usize, align: usize) -> usize {
    (value + align - 1) & !(align - 1)
}

pub(crate) fn init_percpu_areas() -> Result<(), ()> {
    if PERCPU_BASE.load(Ordering::Acquire) != 0 {
        return Ok(());
    }
    let template_start = ptr::addr_of!(_percpu_load_start) as usize;
    let template_end = ptr::addr_of!(_percpu_load_end) as usize;
    let template_size = template_end.checked_sub(template_start).ok_or(())?;
    if template_size == 0 {
        return Err(());
    }
    let stride = align_up(template_size, 64);
    let cpus = unsafe { fh_host_cpu_count() };
    let layout =
        Layout::from_size_align(stride.checked_mul(cpus).ok_or(())?, 64).map_err(|_| ())?;
    let base = unsafe { ALLOCATOR.alloc_zeroed(layout) };
    if base.is_null() {
        return Err(());
    }
    for cpu in 0..cpus {
        unsafe {
            ptr::copy_nonoverlapping(
                template_start as *const u8,
                base.add(cpu * stride),
                template_size,
            );
        }
    }
    PERCPU_STRIDE.store(stride, Ordering::Release);
    PERCPU_BASE.store(base as usize, Ordering::Release);
    let mut output = Console;
    let _ = writeln!(
        output,
        "FH: ax-percpu template={} stride={} cpus={} base={:#x}",
        template_size, stride, cpus, base as usize
    );
    Ok(())
}

#[unsafe(no_mangle)]
pub extern "C" fn _percpu_base_ptr(cpu: usize) -> *mut u8 {
    let base = PERCPU_BASE.load(Ordering::Acquire);
    let stride = PERCPU_STRIDE.load(Ordering::Acquire);
    (base + cpu * stride) as *mut u8
}

#[unsafe(no_mangle)]
pub extern "C" fn ax_percpu_current_base() -> usize {
    let cpu = unsafe { fh_current_cpu_id() };
    _percpu_base_ptr(cpu) as usize
}
