/* SPDX-License-Identifier: Apache-2.0 */
#include <freehypervisor/zephyr_adapter.h>

#include <zephyr/arch/x86/msr.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include <errno.h>
#include <string.h>

static void finish(const char *marker)
{
	printk("%s\n", marker);
	if (IS_ENABLED(CONFIG_FREEHYPERVISOR_REBOOT_ON_FINISH)) {
		sys_reboot(SYS_REBOOT_COLD);
	}
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

#if defined(CONFIG_FREEHYPERVISOR_MODE_PROBE) || \
	defined(CONFIG_FREEHYPERVISOR_MODE_CORE) || \
	defined(CONFIG_FREEHYPERVISOR_MODE_KICK)
static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b,
		  uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid"
			 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			 : "a"(leaf), "c"(subleaf));
}

static bool vmx_probe(void)
{
	uint32_t a, b, c, d;
	char vendor[13];
	cpuid(0, 0, &a, &b, &c, &d);
	memcpy(&vendor[0], &b, 4);
	memcpy(&vendor[4], &d, 4);
	memcpy(&vendor[8], &c, 4);
	vendor[12] = '\0';
	printk("FH_PROBE vendor=%s max_basic=%#x\n", vendor, a);
	uint32_t max_basic = a;
	bool vmx = false;
	if (max_basic >= 1U) {
		cpuid(1, 0, &a, &b, &c, &d);
		vmx = (c & BIT(5)) != 0;
		printk("FH_PROBE CPUID.1h.ECX=%#x VMX=%s\n", c,
		       vmx ? "yes" : "no");
	}
	cpuid(0x80000000U, 0, &a, &b, &c, &d);
	uint32_t max_ext = a;
	bool svm = false;
	if (max_ext >= 0x80000001U) {
		cpuid(0x80000001U, 0, &a, &b, &c, &d);
		svm = (c & BIT(2)) != 0;
		printk("FH_PROBE CPUID.80000001h.ECX=%#x SVM=%s\n", c,
		       svm ? "yes" : "no");
	}
	if (svm && max_ext >= 0x8000000aU) {
		cpuid(0x8000000aU, 0, &a, &b, &c, &d);
		printk("FH_PROBE SVM revision=%u ASIDs=%u features=%#x NPT=%s\n",
		       a & 0xffU, b, d, (d & BIT(0)) != 0 ? "yes" : "no");
		printk("FH_PROBE EFER=%#llx VM_CR=%#llx VM_HSAVE_PA=%#llx\n",
		       (unsigned long long)z_x86_msr_read(0xc0000080U),
		       (unsigned long long)z_x86_msr_read(0xc0010114U),
		       (unsigned long long)z_x86_msr_read(0xc0010117U));
	}
	printk("FH_PROBE SVM_AVAILABLE=%s VMX_AVAILABLE=%s\n",
	       svm ? "yes" : "no", vmx ? "yes" : "no");
	return vmx;
}
#endif

#if defined(CONFIG_FREEHYPERVISOR_CONTRACT_TESTS)
static atomic_t contract_irq_vector = ATOMIC_INIT(-1);

static void contract_irq_handler(size_t vector)
{
	atomic_set(&contract_irq_vector, (atomic_val_t)vector);
}

struct predicate_context {
	size_t queue;
	atomic_t condition;
	atomic_t complete;
};

static bool predicate(void *opaque)
{
	struct predicate_context *context = opaque;
	return atomic_get(&context->condition) != 0;
}

static void predicate_waiter(void *opaque)
{
	struct predicate_context *context = opaque;
	fh_wait_queue_wait_until(context->queue, predicate, context);
	atomic_set(&context->complete, 1);
}

static void raw_waiter(void *opaque)
{
	struct predicate_context *context = opaque;
	fh_wait_queue_wait(context->queue);
	atomic_set(&context->complete, 1);
}

struct task_observation {
	atomic_t cpu;
	atomic_t handle_seen;
};

static void observe_task(void *opaque)
{
	struct task_observation *observation = opaque;
	atomic_set(&observation->cpu, (atomic_val_t)fh_current_cpu_id());
	atomic_set(&observation->handle_seen, (atomic_val_t)fh_task_current());
	fh_task_yield();
}

struct free_context {
	uint64_t pa;
};

static void paused_free(void *opaque)
{
	struct free_context *context = opaque;
	fh_free_frames(context->pa, 1);
}

static bool contract_tests(bool lifetime_mode)
{
	bool pass = true;
	printk("FH_TEST contract begin\n");

	uint8_t console_byte = 0;
	bool platform_pass = fh_host_cpu_count() == 2 &&
		fh_current_cpu_id() < fh_host_cpu_count() &&
		fh_host_tsc_frequency_mhz() != 0 &&
		fh_console_read(&console_byte, sizeof(console_byte)) == 0;
	pass &= platform_pass;
	printk("FH_TEST platform cpus=%zu current=%zu tsc_mhz=%u console-read=0 %s\n",
	       fh_host_cpu_count(), fh_current_cpu_id(), fh_host_tsc_frequency_mhz(),
	       platform_pass ? "PASS" : "FAIL");

	uint64_t pa = fh_alloc_frames(2, 2 * 4096);
	void *va = fh_phys_to_virt(pa);
	pass &= pa != 0 && va != NULL && fh_virt_to_phys(va) == pa &&
		(pa % (2 * 4096)) == 0;
	if (va != NULL) {
		memset(va, 0xa5, 8192);
	}
	fh_free_frames(pa, 2);
	printk("FH_TEST memory round-trip %s\n", pass ? "PASS" : "FAIL");

	struct task_observation observation = {0};
	size_t task = fh_task_spawn((const uint8_t *)"contract-task", 13, 8192,
				    BIT(1), observe_task, &observation);
	fh_task_join(task);
	bool task_pass = task != 0 && atomic_get(&observation.cpu) == 1 &&
			 atomic_get(&observation.handle_seen) == (atomic_val_t)task;
	pass &= task_pass;
	printk("FH_TEST execution context cpu=%ld handle=%zu %s\n",
	       (long)atomic_get(&observation.cpu), task, task_pass ? "PASS" : "FAIL");

	const size_t contract_vector = CONFIG_FREEHYPERVISOR_IPI_VECTOR + 1;
	bool irq_pass = fh_irq_register(contract_vector, contract_irq_handler) &&
		fh_irq_handle(contract_vector) &&
		atomic_get(&contract_irq_vector) == (atomic_val_t)contract_vector &&
		!fh_irq_handle(255);
	pass &= irq_pass;
	printk("FH_TEST irq register/dispatch vector=%zu %s\n", contract_vector,
	       irq_pass ? "PASS" : "FAIL");

	struct predicate_context before = {.queue = fh_wait_queue_create()};
	atomic_set(&before.condition, 1);
	fh_wait_queue_wake_one(before.queue);
	fh_wait_queue_wait_until(before.queue, predicate, &before);
	fh_wait_queue_destroy(before.queue);
	printk("FH_TEST publish-before-install PASS\n");

	struct predicate_context window = {.queue = fh_wait_queue_create()};
	fh_test_wait_pause_enable(true);
	size_t waiter = fh_task_spawn((const uint8_t *)"window-wait", 11, 8192,
				      BIT(1), predicate_waiter, &window);
	for (int i = 0; i < 100 && !fh_test_wait_pause_entered(); ++i) {
		k_sleep(K_MSEC(1));
	}
	bool entered = fh_test_wait_pause_entered();
	atomic_set(&window.condition, 1);
	fh_wait_queue_wake_one(window.queue);
	fh_test_wait_pause_release();
	fh_test_wait_pause_enable(false);
	fh_task_join(waiter);
	bool window_pass = entered && atomic_get(&window.complete) != 0;
	pass &= window_pass;
	fh_wait_queue_destroy(window.queue);
	printk("FH_TEST post-install/pre-sleep %s\n", window_pass ? "PASS" : "FAIL");

	struct predicate_context stale = {.queue = fh_wait_queue_create()};
	fh_wait_queue_wake_one(stale.queue);
	waiter = fh_task_spawn((const uint8_t *)"stale-credit", 12, 8192,
			       BIT(1), predicate_waiter, &stale);
	k_sleep(K_MSEC(5));
	bool remained_blocked = atomic_get(&stale.complete) == 0;
	atomic_set(&stale.condition, 1);
	fh_wait_queue_wake_one(stale.queue);
	fh_task_join(waiter);
	bool stale_pass = remained_blocked && atomic_get(&stale.complete) != 0;
	pass &= stale_pass;
	fh_wait_queue_destroy(stale.queue);
	printk("FH_TEST stale-credit rejection %s\n", stale_pass ? "PASS" : "FAIL");

	struct predicate_context all_a = {.queue = fh_wait_queue_create()};
	struct predicate_context all_b = {.queue = all_a.queue};
	size_t waiter_a = fh_task_spawn((const uint8_t *)"wake-all-a", 10, 8192,
					BIT(0), predicate_waiter, &all_a);
	size_t waiter_b = fh_task_spawn((const uint8_t *)"wake-all-b", 10, 8192,
					BIT(1), predicate_waiter, &all_b);
	k_sleep(K_MSEC(5));
	atomic_set(&all_a.condition, 1);
	atomic_set(&all_b.condition, 1);
	fh_wait_queue_wake_all(all_a.queue);
	fh_task_join(waiter_a);
	fh_task_join(waiter_b);
	bool all_pass = atomic_get(&all_a.complete) != 0 && atomic_get(&all_b.complete) != 0;
	pass &= all_pass;
	fh_wait_queue_destroy(all_a.queue);
	printk("FH_TEST wake-all %s\n", all_pass ? "PASS" : "FAIL");

	struct predicate_context closing = {.queue = fh_wait_queue_create()};
	waiter = fh_task_spawn((const uint8_t *)"close-drain", 11, 8192,
			       BIT(1), raw_waiter, &closing);
	k_sleep(K_MSEC(5));
	fh_wait_queue_destroy(closing.queue);
	fh_task_join(waiter);
	bool close_pass = atomic_get(&closing.complete) != 0;
	pass &= close_pass;
	printk("FH_TEST close/drain %s\n", close_pass ? "PASS" : "FAIL");

	fh_test_timer_suppress_callback(true);
	uint64_t deadline = fh_monotonic_nanos() + 5 * 1000 * 1000ULL;
	fh_set_oneshot_timer(deadline);
	for (int i = 0; i < 100 && fh_test_timer_expiry_count() == 0; ++i) {
		k_sleep(K_MSEC(1));
	}
	uint64_t count = fh_test_timer_expiry_count();
	uint64_t fired = fh_test_timer_last_fire_ns();
	k_sleep(K_MSEC(10));
	bool timer_pass = count == 1 && fh_test_timer_expiry_count() == 1 && fired >= deadline;
	pass &= timer_pass;
	printk("FH_TEST absolute one-shot timer deadline=%llu fired=%llu count=%llu %s\n",
	       deadline, fired, count, timer_pass ? "PASS" : "FAIL");

	uint64_t whole_pool = fh_alloc_frames(fh_pool_page_count(), 1);
	struct free_context free_context = {.pa = whole_pool};
	fh_test_free_pause_enable(true);
	size_t freer = fh_task_spawn((const uint8_t *)"paused-free", 11, 8192,
				     BIT(1), paused_free, &free_context);
	for (int i = 0; i < 100 && !fh_test_free_pause_entered(); ++i) {
		k_sleep(K_MSEC(1));
	}
	uint64_t early = fh_alloc_frames(1, 1);
	fh_test_free_pause_release();
	fh_test_free_pause_enable(false);
	fh_task_join(freer);
	uint64_t after = fh_alloc_frames(1, 1);
	bool ownership_pass = whole_pool != 0 && early == 0 && after == whole_pool;
	pass &= ownership_pass;
	if (after != 0) {
		fh_free_frames(after, 1);
	}
	if (whole_pool != 0) {
		fh_free_frames(whole_pool + 4096, fh_pool_page_count() - 1);
	}
	printk("FH_TEST free/reallocate ownership handoff %s\n",
	       ownership_pass ? "PASS" : "FAIL");

	if (lifetime_mode) {
		struct k_sem broken_sem;
		atomic_t broken_waiter = ATOMIC_INIT(0);
		k_sem_init(&broken_sem, 0, 1);
		/* Broken producer: a wake before waiter publication is discarded. */
		if (atomic_get(&broken_waiter) != 0) {
			k_sem_give(&broken_sem);
		}
		atomic_set(&broken_waiter, 1);
		bool negative_exposed = k_sem_take(&broken_sem, K_MSEC(5)) == -EAGAIN;
		pass &= negative_exposed;
		printk("FH_TEST deliberately-broken lost-wake negative-control %s\n",
		       negative_exposed ? "PASS" : "FAIL");
	}

	printk("FH_TEST contract result=%s\n", pass ? "PASS" : "FAIL");
	return pass;
}
#endif

#if defined(CONFIG_FREEHYPERVISOR_MODE_KICK)
K_THREAD_STACK_DEFINE(kick_stack, 4096);
static struct k_thread kick_thread;

static void kick_sender(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	/* Allow static VM setup to finish, then interrupt the long guest loop. */
	k_sleep(K_MSEC(100));
	for (int i = 0; i < 3; ++i) {
		printk("FH_KICK sender_cpu=%zu target_cpu=0 attempt=%d\n",
		       fh_current_cpu_id(), i + 1);
		fh_hv_ipi_send(0);
		k_sleep(K_MSEC(10));
	}
}
#endif

int main(void)
{
	printk("FH: Zephyr host adapter start cpus=%zu current_cpu=%zu\n",
	       fh_host_cpu_count(), fh_current_cpu_id());
	if (fh_adapter_init() != 0) {
		finish("FH_RESULT adapter-init result=FAIL");
	}

#if defined(CONFIG_FREEHYPERVISOR_MODE_CONTRACT)
	finish(contract_tests(false) ? "FH_RESULT contract result=PASS" :
				       "FH_RESULT contract result=FAIL");
#elif defined(CONFIG_FREEHYPERVISOR_MODE_LIFETIME)
	finish(contract_tests(true) ? "FH_RESULT lifetime result=PASS" :
				      "FH_RESULT lifetime result=FAIL");
#elif defined(CONFIG_FREEHYPERVISOR_MODE_PROBE)
	finish(vmx_probe() ? "FH_RESULT vmx-probe result=PASS" :
			     "FH_RESULT vmx-probe result=BLOCKED");
#elif defined(CONFIG_FREEHYPERVISOR_MODE_CORE) || defined(CONFIG_FREEHYPERVISOR_MODE_KICK)
	if (!vmx_probe()) {
		finish("FH_RESULT core result=BLOCKED_NO_VMX");
	}
	if (fh_rust_init() != 0) {
		finish("FH_RESULT rust-init result=FAIL");
	}
#if defined(CONFIG_FREEHYPERVISOR_MODE_KICK)
	k_tid_t kick = k_thread_create(&kick_thread, kick_stack,
				       K_THREAD_STACK_SIZEOF(kick_stack), kick_sender,
				       NULL, NULL, NULL, K_PRIO_PREEMPT(2), 0, K_FOREVER);
	k_thread_cpu_mask_clear(kick);
	k_thread_cpu_mask_enable(kick, 1);
	k_thread_start(kick);
#endif
	fh_rust_run();
	fh_time_shutdown();
#if defined(CONFIG_FREEHYPERVISOR_MODE_KICK)
	k_thread_join(&kick_thread, K_FOREVER);
	bool kick_pass = fh_hv_ipi_count() > 0 && fh_hv_ipi_last_cpu() == 0;
	printk("FH_KICK count=%llu last_cpu=%u result=%s\n",
	       fh_hv_ipi_count(), fh_hv_ipi_last_cpu(), kick_pass ? "PASS" : "FAIL");
	finish(kick_pass ? "FH_RESULT inguest-ipi result=PASS" :
			   "FH_RESULT inguest-ipi result=FAIL");
#else
	finish("FH_RESULT core-static result=PASS");
#endif
#else
	finish("FH_RESULT unknown-mode result=FAIL");
#endif
	return 0;
}
