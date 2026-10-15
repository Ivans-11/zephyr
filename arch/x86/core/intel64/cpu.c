/*
 * Copyright (c) 2019 Intel Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cpuid.h>
#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <kernel_arch_data.h>
#include <kernel_arch_func.h>
#include <zephyr/kernel_structs.h>
#include <kernel_internal.h>
#include <zephyr/arch/x86/multiboot.h>
#include <x86_mmu.h>
#include <zephyr/drivers/interrupt_controller/loapic.h>
#include <cet.h>
#include <zephyr/arch/common/xip.h>
#include <zephyr/arch/common/init.h>
#ifdef CONFIG_ACPI
#include <zephyr/arch/x86/cpuid.h>
#include <zephyr/acpi/acpi.h>
#endif

/*
 * Map of CPU logical IDs to CPU local APIC IDs. By default,
 * we assume this simple identity mapping, as found in QEMU.
 * The symbol is weak so that boards/SoC files can override.
 */

#if defined(CONFIG_ACPI)
__weak uint32_t x86_cpu_loapics[CONFIG_MP_MAX_NUM_CPUS];
unsigned int z_x86_num_cpus = CONFIG_MP_MAX_NUM_CPUS;

/* Discover enabled processors before SMP startup. Keep the BSP at logical 0. */
static int discover_cpus(void)
{
	ACPI_TABLE_HEADER *madt = acpi_table_get("APIC", 0);
	uint32_t bsp = x86_read_loapic(LOAPIC_ID);

	if (!IS_ENABLED(CONFIG_X2APIC)) {
		bsp >>= 24;
	}
	x86_cpu_loapics[0] = bsp;
	z_x86_num_cpus = 1;
	if (madt == NULL || madt->Length < sizeof(ACPI_TABLE_MADT)) {
		printk("ACPI: no valid MADT; using BSP only\n");
		return 0;
	}
	for (size_t off = sizeof(ACPI_TABLE_MADT);
	     off + sizeof(ACPI_SUBTABLE_HEADER) <= madt->Length;) {
		ACPI_SUBTABLE_HEADER *entry = (void *)((uint8_t *)madt + off);
		uint32_t id, flags;

		if (entry->Length < sizeof(*entry) || entry->Length > madt->Length - off) {
			printk("ACPI: malformed MADT CPU entry\n");
			break;
		}
		off += entry->Length;
		if (entry->Type == ACPI_MADT_TYPE_LOCAL_APIC &&
		    entry->Length >= sizeof(ACPI_MADT_LOCAL_APIC)) {
			ACPI_MADT_LOCAL_APIC *cpu = (void *)entry;

			id = cpu->Id;
			flags = cpu->LapicFlags;
		} else if (entry->Type == ACPI_MADT_TYPE_LOCAL_X2APIC &&
			   entry->Length >= sizeof(ACPI_MADT_LOCAL_X2APIC)) {
			ACPI_MADT_LOCAL_X2APIC *cpu = (void *)entry;

			id = cpu->LocalApicId;
			flags = cpu->LapicFlags;
		} else {
			continue;
		}
		if (!(flags & 1U) || (!IS_ENABLED(CONFIG_X2APIC) && id > 255)) {
			continue;
		}
		bool duplicate = false;

		for (unsigned int i = 0; i < z_x86_num_cpus; i++) {
			duplicate |= x86_cpu_loapics[i] == id;
		}
		if (!duplicate && z_x86_num_cpus < CONFIG_MP_MAX_NUM_CPUS) {
			x86_cpu_loapics[z_x86_num_cpus++] = id;
		}
	}
	printk("ACPI: %u enabled host CPUs (limit %u)\n",
	       z_x86_num_cpus, CONFIG_MP_MAX_NUM_CPUS);
	return 0;
}
SYS_INIT(discover_cpus, POST_KERNEL, 0);
#else
#define INIT_CPUID(n, _) n
__weak uint32_t x86_cpu_loapics[] = {
	LISTIFY(CONFIG_MP_MAX_NUM_CPUS, INIT_CPUID, (,)),};
#endif
extern char x86_ap_start[]; /* AP entry point in locore.S */

LISTIFY(CONFIG_MP_MAX_NUM_CPUS, ACPI_CPU_INIT, (;));

Z_GENERIC_SECTION(.boot_arg)
x86_boot_arg_t x86_cpu_boot_arg;

struct x86_cpuboot x86_cpuboot[] = {
	LISTIFY(CONFIG_MP_MAX_NUM_CPUS, X86_CPU_BOOT_INIT, (,)),
};

#ifdef CONFIG_HW_SHADOW_STACK
#define _CPU_IDX(n, _) n
FOR_EACH(X86_INTERRUPT_SHADOW_STACK_DEFINE, (;),
	 LISTIFY(CONFIG_MP_MAX_NUM_CPUS, _CPU_IDX, (,)));

struct x86_interrupt_ssp_table issp_table[] = {
	LISTIFY(CONFIG_MP_MAX_NUM_CPUS, X86_INTERRUPT_SSP_TABLE_INIT, (,)),
};
#endif

/*
 * Send the INIT/STARTUP IPI sequence required to start up CPU 'cpu_num', which
 * will enter the kernel at fn(arg), running on the specified stack.
 */

void arch_cpu_start(int cpu_num, k_thread_stack_t *stack, int sz,
		    arch_cpustart_t fn, void *arg)
{
#if CONFIG_MP_MAX_NUM_CPUS > 1
	uint8_t vector = ((unsigned long) x86_ap_start) >> 12;
	uint32_t apic_id;

	apic_id = x86_cpu_loapics[cpu_num];

	x86_cpuboot[cpu_num].sp = (uint64_t) K_KERNEL_STACK_BUFFER(stack) + sz;
	x86_cpuboot[cpu_num].stack_size = sz;
	x86_cpuboot[cpu_num].fn = fn;
	x86_cpuboot[cpu_num].arg = arg;
	x86_cpuboot[cpu_num].cpu_id = cpu_num;

	z_loapic_ipi(apic_id, LOAPIC_ICR_IPI_INIT, 0);
	k_busy_wait(10000);
	z_loapic_ipi(apic_id, LOAPIC_ICR_IPI_STARTUP, vector);

	while (x86_cpuboot[cpu_num].ready == 0) {
	}
#else
	ARG_UNUSED(cpu_num);
	ARG_UNUSED(stack);
	ARG_UNUSED(sz);
	ARG_UNUSED(fn);
	ARG_UNUSED(arg);
#endif
}

/* Per-CPU initialization, C domain. On the first CPU, z_prep_c is the
 * next step. For other CPUs it is probably smp_init_top().
 */
FUNC_NORETURN void z_x86_cpu_init(struct x86_cpuboot *cpuboot)
{
	x86_sse_init(NULL);

	if (cpuboot->cpu_id == 0U) {
		/* Only need to do these once per boot */
		arch_bss_zero();
		arch_data_copy();
	}

	z_loapic_enable(cpuboot->cpu_id);

#if defined(CONFIG_ACPI) && !defined(CONFIG_ACRN_COMMON)
	/* The BSP discovers its mapping later, during ACPI initialization. */
	if (cpuboot->cpu_id != 0U) {
		__ASSERT((IS_ENABLED(CONFIG_X2APIC) ? x86_read_loapic(LOAPIC_ID) :
			 (x86_read_loapic(LOAPIC_ID) >> 24)) ==
			 x86_cpu_loapics[cpuboot->cpu_id], "APIC ID mismatch!");
	}
#endif

#ifdef CONFIG_USERSPACE
	/* Set landing site for 'syscall' instruction */
	z_x86_msr_write(X86_LSTAR_MSR, (uint64_t)z_x86_syscall_entry_stub);

	/* Set segment descriptors for syscall privilege transitions */
	z_x86_msr_write(X86_STAR_MSR, (uint64_t)X86_STAR_UPPER << 32);

	/* Mask applied to RFLAGS when making a syscall */
	z_x86_msr_write(X86_FMASK_MSR, EFLAGS_SYSCALL);
#endif

#ifdef CONFIG_X86_CET
	z_x86_cet_enable();
#ifdef CONFIG_X86_CET_IBT
	z_x86_ibt_enable();
#endif /* CONFIG_X86_CET_IBT */
#ifdef CONFIG_HW_SHADOW_STACK
	z_x86_setup_interrupt_ssp_table((uintptr_t)&issp_table[cpuboot->cpu_id]);
	cpuboot->gs_base->shstk_addr = &issp_table[cpuboot->cpu_id].ist1;
	cpuboot->gs_base->exception_shstk_addr = issp_table[cpuboot->cpu_id].ist7;
#endif /* CONFIG_HW_SHADOW_STACK */

#endif /* CONFIG_X86_CET */

	/* Enter kernel, never return */
	cpuboot->ready++;
	cpuboot->fn(cpuboot->arg);

	CODE_UNREACHABLE; /* LCOV_EXCL_LINE */
}
