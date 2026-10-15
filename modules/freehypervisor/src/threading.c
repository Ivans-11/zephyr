/* SPDX-License-Identifier: Apache-2.0 */
#include <freehypervisor/zephyr_adapter.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <errno.h>
#include <string.h>

#define FH_MAX_TASKS 32
#define FH_TASK_NAME_LEN 32

struct fh_task {
	bool used;
	size_t handle;
	struct k_thread thread;
	k_thread_stack_t *stack;
	size_t stack_size;
	struct k_sem complete;
	fh_entry_fn entry;
	void *context;
	char name[FH_TASK_NAME_LEN];
	uint64_t run_count;
	uint64_t exit_count;
	uint64_t block_count;
	uint64_t wake_count;
	uint64_t yield_count;
};

static struct fh_task fh_tasks[FH_MAX_TASKS];
static struct k_spinlock fh_tasks_lock;
static size_t fh_next_task = 1;

static void fh_task_trampoline(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	struct fh_task *task = p1;
	task->run_count++;
	task->entry(task->context);
	task->exit_count++;
	k_sem_give(&task->complete);
}

static struct fh_task *fh_task_by_handle(size_t handle)
{
	for (size_t i = 0; i < FH_MAX_TASKS; ++i) {
		if (fh_tasks[i].used && fh_tasks[i].handle == handle) {
			return &fh_tasks[i];
		}
	}
	return NULL;
}

static struct fh_task *fh_task_by_current(void)
{
	k_tid_t current = k_current_get();

	for (size_t i = 0; i < FH_MAX_TASKS; ++i) {
		if (fh_tasks[i].used && &fh_tasks[i].thread == current) {
			return &fh_tasks[i];
		}
	}
	return NULL;
}

size_t fh_task_spawn(const uint8_t *name, size_t name_len, size_t stack_size,
		     size_t cpu_mask, fh_entry_fn entry, void *context)
{
	struct fh_task *task = NULL;
	k_spinlock_key_t key = k_spin_lock(&fh_tasks_lock);
	for (size_t i = 0; i < FH_MAX_TASKS; ++i) {
		if (!fh_tasks[i].used) {
			task = &fh_tasks[i];
			memset(task, 0, sizeof(*task));
			task->used = true;
			task->handle = fh_next_task++;
			break;
		}
	}
	k_spin_unlock(&fh_tasks_lock, key);
	if (task == NULL) {
		return 0;
	}

	if (stack_size < 4096) {
		stack_size = 4096;
	}
	task->stack = k_thread_stack_alloc(stack_size, 0);
	if (task->stack == NULL) {
		task->used = false;
		return 0;
	}
	task->stack_size = stack_size;
	task->entry = entry;
	task->context = context;
	name_len = MIN(name_len, sizeof(task->name) - 1);
	memcpy(task->name, name, name_len);
	task->name[name_len] = '\0';
	k_sem_init(&task->complete, 0, 1);

	k_tid_t tid = k_thread_create(&task->thread, task->stack, task->stack_size,
				      fh_task_trampoline, task, NULL, NULL,
				      K_PRIO_PREEMPT(CONFIG_MAIN_THREAD_PRIORITY), 0,
				      K_FOREVER);
	if (cpu_mask != 0) {
		k_thread_cpu_mask_clear(tid);
		for (unsigned int cpu = 0; cpu < arch_num_cpus(); ++cpu) {
			if ((cpu_mask & BIT(cpu)) != 0) {
				k_thread_cpu_mask_enable(tid, cpu);
			}
		}
	}
	k_thread_name_set(tid, task->name);
	printk("FH: task create handle=%zu name=%s mask=%#zx\n",
	       task->handle, task->name, cpu_mask);
	k_thread_start(tid);
	return task->handle;
}

void fh_task_join(size_t handle)
{
	struct fh_task *task = fh_task_by_handle(handle);
	if (task == NULL) {
		return;
	}
	k_sem_take(&task->complete, K_FOREVER);
	k_thread_join(&task->thread, K_FOREVER);
	printk("FH_TASK_STATS handle=%zu name=%s run=%llu exit=%llu block=%llu wake=%llu yield=%llu\n",
	       task->handle, task->name, task->run_count, task->exit_count,
	       task->block_count, task->wake_count, task->yield_count);
	k_thread_stack_free(task->stack);
	k_spinlock_key_t key = k_spin_lock(&fh_tasks_lock);
	task->used = false;
	k_spin_unlock(&fh_tasks_lock, key);
}

size_t fh_task_current(void)
{
	struct fh_task *task = fh_task_by_current();

	return task != NULL ? task->handle : 0;
}

void fh_task_yield(void)
{
	struct fh_task *task = fh_task_by_current();
	if (task != NULL) {
		task->yield_count++;
	}
	k_yield();
}

void fh_task_note_block(void)
{
	struct fh_task *task = fh_task_by_current();
	if (task != NULL) {
		task->block_count++;
	}
}

void fh_task_note_wake(void)
{
	struct fh_task *task = fh_task_by_current();
	if (task != NULL) {
		task->wake_count++;
	}
}
