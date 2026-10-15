/* SPDX-License-Identifier: Apache-2.0 */
#include <freehypervisor/zephyr_adapter.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

struct fh_cpu_timer {
	struct k_timer timer;
	struct k_work work;
	struct k_spinlock lock;
	unsigned int target_cpu;
	uint64_t deadline_ns;
	bool armed;
	atomic_t arm_generation;
	atomic_t expired_generation;
	atomic_t callback_generation;
	atomic_t retry_count;
};

static struct fh_cpu_timer fh_oneshot[CONFIG_MP_MAX_NUM_CPUS];
static atomic_t fh_timer_kick_reported;
static atomic_t fh_timer_stopping;

#define FH_TIMER_KICK_RETRIES 50

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
static atomic_t fh_timer_suppress;
static atomic_t fh_timer_count;
static atomic_t fh_timer_last_fire;
#endif

uint64_t fh_monotonic_nanos(void)
{
	return k_ticks_to_ns_floor64(k_uptime_ticks());
}

static void fh_timer_expiry(struct k_timer *timer)
{
	struct fh_cpu_timer *cpu_timer =
		CONTAINER_OF(timer, struct fh_cpu_timer, timer);
	uint64_t now = fh_monotonic_nanos();
	k_spinlock_key_t key = k_spin_lock(&cpu_timer->lock);
	if (cpu_timer->armed && now < cpu_timer->deadline_ns) {
		k_spin_unlock(&cpu_timer->lock, key);
		return;
	}
	cpu_timer->armed = false;
	k_spin_unlock(&cpu_timer->lock, key);
#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
	atomic_inc(&fh_timer_count);
	atomic_set(&fh_timer_last_fire, (atomic_val_t)now);
	if (atomic_get(&fh_timer_suppress) != 0) {
		return;
	}
#endif
	if (atomic_get(&fh_timer_stopping) != 0) {
		return;
	}

	atomic_set(&cpu_timer->expired_generation,
		   atomic_get(&cpu_timer->arm_generation));
	if (atomic_inc(&cpu_timer->retry_count) + 1 >= FH_TIMER_KICK_RETRIES) {
		k_timer_stop(timer);
	}
	(void)k_work_submit(&cpu_timer->work);
}

static void fh_timer_work(struct k_work *work)
{
	struct fh_cpu_timer *cpu_timer =
		CONTAINER_OF(work, struct fh_cpu_timer, work);
	atomic_val_t generation;
	atomic_val_t delivered;

	if (atomic_get(&fh_timer_stopping) != 0) {
		return;
	}

	generation = atomic_get(&cpu_timer->expired_generation);
	if (generation != atomic_get(&cpu_timer->arm_generation)) {
		return;
	}

#ifdef CONFIG_FREEHYPERVISOR_CORE
	delivered = atomic_get(&cpu_timer->callback_generation);
	if (delivered != generation &&
	    atomic_cas(&cpu_timer->callback_generation, delivered, generation)) {
		/* Rust timer handling can take locks, so it must run in thread context. */
		fh_rust_timer_expired();
	}
#else
	ARG_UNUSED(delivered);
#endif

#ifdef CONFIG_FREEHYPERVISOR_DEDICATED_IPI
	/*
	 * Zephyr k_timer callbacks may execute on a different CPU from the
	 * caller that armed the timer.  The Core uses this one-shot as a
	 * bounded VM-exit poll point (for example, to inject a due virtual
	 * PIT tick).  Always send the dedicated IPI to the owning CPU: an
	 * outer hypervisor may service the physical timer transparently and
	 * resume nested execution without reporting an SVM/VMX interrupt exit
	 * to this Core, even when the callback happened on the same pCPU.
	 */
	(void)fh_hv_ipi_send(cpu_timer->target_cpu);
	if (atomic_cas(&fh_timer_kick_reported, 0, 1)) {
		printk("FH: timer VM-exit kick target_cpu=%u callback_cpu=%zu\n",
		       cpu_timer->target_cpu, fh_current_cpu_id());
	}
#endif
}

int fh_time_init(void)
{
	for (unsigned int cpu = 0; cpu < ARRAY_SIZE(fh_oneshot); ++cpu) {
		fh_oneshot[cpu].target_cpu = cpu;
		k_timer_init(&fh_oneshot[cpu].timer, fh_timer_expiry, NULL);
		k_work_init(&fh_oneshot[cpu].work, fh_timer_work);
		atomic_set(&fh_oneshot[cpu].callback_generation, -1);
	}
	return 0;
}

void fh_time_shutdown(void)
{
	struct k_work_sync sync;

	atomic_set(&fh_timer_stopping, 1);
	for (unsigned int cpu = 0; cpu < ARRAY_SIZE(fh_oneshot); ++cpu) {
		k_timer_stop(&fh_oneshot[cpu].timer);
		(void)k_work_cancel_sync(&fh_oneshot[cpu].work, &sync);
	}
}

void fh_set_oneshot_timer(uint64_t deadline_ns)
{
	unsigned int cpu = (unsigned int)fh_current_cpu_id();
	uint64_t now = fh_monotonic_nanos();
	uint64_t delay;
	k_spinlock_key_t key;

	if (cpu >= ARRAY_SIZE(fh_oneshot)) {
		cpu = 0;
	}
	key = k_spin_lock(&fh_oneshot[cpu].lock);
	if (fh_oneshot[cpu].armed &&
	    fh_oneshot[cpu].deadline_ns <= deadline_ns) {
		k_spin_unlock(&fh_oneshot[cpu].lock, key);
		return;
	}
	fh_oneshot[cpu].deadline_ns = deadline_ns;
	fh_oneshot[cpu].armed = true;
	atomic_inc(&fh_oneshot[cpu].arm_generation);
	atomic_set(&fh_oneshot[cpu].retry_count, 0);
	k_spin_unlock(&fh_oneshot[cpu].lock, key);
	delay = deadline_ns > now ? deadline_ns - now : 1;
#ifdef CONFIG_FREEHYPERVISOR_CORE
	/*
	 * The callback is one-shot per generation.  The 1 ms period only retries
	 * the dedicated VM-exit kick when an outer hypervisor consumed an IPI
	 * outside the narrow VMRUN window.
	 */
	k_timer_start(&fh_oneshot[cpu].timer, K_NSEC(delay), K_MSEC(1));
#else
	k_timer_start(&fh_oneshot[cpu].timer, K_NSEC(delay), K_NO_WAIT);
#endif
}

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
void fh_test_timer_suppress_callback(bool suppress)
{
	atomic_set(&fh_timer_suppress, suppress ? 1 : 0);
	atomic_set(&fh_timer_count, 0);
	atomic_set(&fh_timer_last_fire, 0);
}

uint64_t fh_test_timer_expiry_count(void)
{
	return (uint64_t)atomic_get(&fh_timer_count);
}

uint64_t fh_test_timer_last_fire_ns(void)
{
	return (uint64_t)atomic_get(&fh_timer_last_fire);
}
#endif
