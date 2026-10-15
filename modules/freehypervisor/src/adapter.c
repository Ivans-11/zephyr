/* SPDX-License-Identifier: Apache-2.0 */
#include <freehypervisor/zephyr_adapter.h>

#include <zephyr/arch/x86/msr.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/timing/timing.h>

#include <errno.h>

#if defined(CONFIG_X86_64)
void fh_prepare_x86_virtualization(void)
{
	uint64_t cr0;
	uint64_t cr4;
	uint64_t cr0_fixed0 = z_x86_msr_read(0x486U);
	uint64_t cr0_fixed1 = z_x86_msr_read(0x487U);
	uint64_t cr4_fixed0 = z_x86_msr_read(0x488U);
	uint64_t cr4_fixed1 = z_x86_msr_read(0x489U);

	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
	/* Normalize per-CPU control registers before the core executes VMXON. */
	cr0 = (cr0 & cr0_fixed1) | cr0_fixed0;
	/* Leave VMXE clear; axvisor-core enables it immediately before VMXON. */
	cr4 = (cr4 & cr4_fixed1) | (cr4_fixed0 & ~BIT64(13));
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
	__asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}
#endif

int fh_memory_init(void);
int fh_time_init(void);
K_MUTEX_DEFINE(fh_console_mutex);

int fh_adapter_init(void)
{
	int rc = fh_memory_init();

	if (rc != 0) {
		return rc;
	}
	timing_init();
	rc = fh_time_init();
	if (rc != 0) {
		return rc;
	}
#ifdef CONFIG_FREEHYPERVISOR_DEDICATED_IPI
	rc = fh_hv_ipi_init();
#endif
	return rc;
}

void fh_console_write(const uint8_t *bytes, size_t length)
{
	for (size_t i = 0; i < length; ++i) {
		char c = (char)bytes[i];
		if (c == '\n') {
			printk("\r");
		}
		printk("%c", c);
	}
}

size_t fh_console_read(uint8_t *bytes, size_t length)
{
	ARG_UNUSED(bytes);
	ARG_UNUSED(length);
	return 0;
}

void fh_console_acquire(void)
{
	k_mutex_lock(&fh_console_mutex, K_FOREVER);
}

void fh_console_release(void)
{
	k_mutex_unlock(&fh_console_mutex);
}

void *fh_rust_alloc(size_t size, size_t align)
{
	if (size == 0) {
		size = 1;
	}
	if (align < sizeof(void *)) {
		align = sizeof(void *);
	}
	return k_aligned_alloc(align, size);
}

void fh_rust_dealloc(void *ptr, size_t size, size_t align)
{
	ARG_UNUSED(size);
	ARG_UNUSED(align);
	k_free(ptr);
}

void fh_rust_abort(void)
{
	printk("FH: Rust panic; rebooting\n");
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

size_t fh_host_cpu_count(void)
{
	return arch_num_cpus();
}

size_t fh_current_cpu_id(void)
{
	return arch_curr_cpu()->id;
}

uint32_t fh_host_tsc_frequency_mhz(void)
{
	return timing_freq_get_mhz();
}
