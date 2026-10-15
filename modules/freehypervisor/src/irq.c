/* SPDX-License-Identifier: Apache-2.0 */
#include <freehypervisor/zephyr_adapter.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/interrupt_controller/ioapic.h>
#include <zephyr/drivers/interrupt_controller/loapic.h>
#include <zephyr/irq.h>
#include <zephyr/sys/printk.h>

#include <errno.h>

#define FH_IRQ_SLOTS 256
#define FH_FIRST_VECTOR 32
#define FH_VECTOR_COUNT (256 - FH_FIRST_VECTOR)
#define FH_IOAPIC_GSI_COUNT 24

extern void (*x86_irq_funcs[FH_VECTOR_COUNT])(const void *arg);
extern const void *x86_irq_args[FH_VECTOR_COUNT];

static fh_irq_fn fh_irq_handlers[FH_IRQ_SLOTS];
static void (*fh_spurious_irq_handler)(const void *arg);
static atomic_t fh_ipi_counter;
static atomic_t fh_ipi_cpu;

static void fh_registered_irq_isr(const void *arg)
{
	size_t vector = (size_t)(uintptr_t)arg;

	(void)fh_irq_handle(vector);
}

bool fh_irq_handle(size_t vector)
{
	if (vector >= ARRAY_SIZE(fh_irq_handlers) || fh_irq_handlers[vector] == NULL) {
		return false;
	}
	fh_irq_handlers[vector](vector);
	return true;
}

bool fh_irq_register(size_t vector, fh_irq_fn handler)
{
	if (vector < FH_FIRST_VECTOR || vector >= ARRAY_SIZE(fh_irq_handlers) ||
	    handler == NULL || fh_irq_handlers[vector] != NULL ||
	    fh_spurious_irq_handler == NULL) {
		return false;
	}

	/*
	 * AxVisor identifies x86 IOAPIC inputs as 0x20 + GSI.  This is not
	 * necessarily the host IDT vector chosen by Zephyr, so let Zephyr allocate
	 * a real vector and program the corresponding IOAPIC line.
	 */
	if (vector < FH_FIRST_VECTOR + FH_IOAPIC_GSI_COUNT) {
		unsigned int gsi = vector - FH_FIRST_VECTOR;
		unsigned int priority = 2 + gsi / 16;
		/* Guest vIOAPIC state retains level semantics; the host consumes each
		 * device assertion once so its EOI cannot starve the vCPU task. */
		int host_vector = irq_connect_dynamic(gsi, priority,
						      fh_registered_irq_isr,
						      (const void *)(uintptr_t)vector,
						      IOAPIC_EDGE | IOAPIC_HIGH);

		if (host_vector < 0) {
			return false;
		}
		fh_irq_handlers[vector] = handler;
		irq_enable(gsi);
		return true;
	}

	unsigned int key = irq_lock();
	if (x86_irq_funcs[vector - FH_FIRST_VECTOR] != fh_spurious_irq_handler) {
		irq_unlock(key);
		return false;
	}
	fh_irq_handlers[vector] = handler;
	x86_irq_funcs[vector - FH_FIRST_VECTOR] = fh_registered_irq_isr;
	x86_irq_args[vector - FH_FIRST_VECTOR] = (const void *)(uintptr_t)vector;
	irq_unlock(key);
	return true;
}

static void fh_hv_ipi_isr(const void *arg)
{
	ARG_UNUSED(arg);
	unsigned int cpu = (unsigned int)fh_current_cpu_id();
	atomic_set(&fh_ipi_cpu, (atomic_val_t)cpu);
	atomic_inc(&fh_ipi_counter);
	(void)fh_irq_handle(CONFIG_FREEHYPERVISOR_IPI_VECTOR);
#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
	printk("FH: dedicated IPI vector=%d cpu=%u count=%lld\n",
	       CONFIG_FREEHYPERVISOR_IPI_VECTOR, cpu,
	       (long long)atomic_get(&fh_ipi_counter));
#else
	if (atomic_get(&fh_ipi_counter) == 1) {
		printk("FH: first dedicated IPI received vector=%d cpu=%u\n",
		       CONFIG_FREEHYPERVISOR_IPI_VECTOR, cpu);
	}
#endif
}

int fh_hv_ipi_init(void)
{
	unsigned int vector = CONFIG_FREEHYPERVISOR_IPI_VECTOR;
	if (vector < FH_FIRST_VECTOR || vector >= 256 ||
	    vector == CONFIG_SCHED_IPI_VECTOR || vector == CONFIG_TLB_IPI_VECTOR) {
		return -EINVAL;
	}
	unsigned int key = irq_lock();
	/*
	 * Zephyr initializes every unused x86 vector to the same private
	 * spurious-IRQ handler.  Capture it before reserving our vectors so the
	 * contract registration path can reject vectors already owned by the
	 * kernel and install a real ISR only into free slots.
	 */
	fh_spurious_irq_handler = x86_irq_funcs[vector + 1 - FH_FIRST_VECTOR];
	x86_irq_funcs[vector - FH_FIRST_VECTOR] = fh_hv_ipi_isr;
	x86_irq_args[vector - FH_FIRST_VECTOR] = NULL;
	irq_unlock(key);
	printk("FH: dedicated IPI vector=%u scheduler=%u tlb=%u\n",
	       vector, CONFIG_SCHED_IPI_VECTOR, CONFIG_TLB_IPI_VECTOR);
	return 0;
}

int fh_hv_ipi_send(unsigned int cpu_id)
{
	if (cpu_id >= arch_num_cpus()) {
		return -EINVAL;
	}
	z_loapic_ipi((uint8_t)cpu_id, LOAPIC_ICR_IPI_SPECIFIC,
		     CONFIG_FREEHYPERVISOR_IPI_VECTOR);
	return 0;
}

uint64_t fh_hv_ipi_count(void)
{
	return (uint64_t)atomic_get(&fh_ipi_counter);
}

unsigned int fh_hv_ipi_last_cpu(void)
{
	return (unsigned int)atomic_get(&fh_ipi_cpu);
}
