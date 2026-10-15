/* SPDX-License-Identifier: Apache-2.0 */
#include <freehypervisor/zephyr_adapter.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <string.h>

#define FH_MAX_WAIT_QUEUES 32

struct fh_wait_queue {
	bool used;
	bool closed;
	uint64_t generation;
	unsigned int waiters;
	struct k_sem semaphore;
	struct k_spinlock lock;
};

static struct fh_wait_queue fh_queues[FH_MAX_WAIT_QUEUES];
static struct k_spinlock fh_queue_table_lock;

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
static atomic_t fh_wait_pause;
static atomic_t fh_wait_entered;
K_SEM_DEFINE(fh_wait_release, 0, 1);
#endif

static struct fh_wait_queue *fh_queue(size_t handle)
{
	if (handle == 0 || handle > FH_MAX_WAIT_QUEUES) {
		return NULL;
	}
	struct fh_wait_queue *queue = &fh_queues[handle - 1];
	return queue->used ? queue : NULL;
}

size_t fh_wait_queue_create(void)
{
	k_spinlock_key_t key = k_spin_lock(&fh_queue_table_lock);
	for (size_t i = 0; i < FH_MAX_WAIT_QUEUES; ++i) {
		if (!fh_queues[i].used) {
			struct fh_wait_queue *queue = &fh_queues[i];
			memset(queue, 0, sizeof(*queue));
			queue->used = true;
			k_sem_init(&queue->semaphore, 0, UINT_MAX);
			k_spin_unlock(&fh_queue_table_lock, key);
			return i + 1;
		}
	}
	k_spin_unlock(&fh_queue_table_lock, key);
	return 0;
}

static void fh_waiter_remove(struct fh_wait_queue *queue)
{
	k_spinlock_key_t key = k_spin_lock(&queue->lock);
	if (queue->waiters > 0) {
		queue->waiters--;
	}
	k_spin_unlock(&queue->lock, key);
}

static void fh_wait_common(struct fh_wait_queue *queue, fh_predicate_fn predicate,
			   void *context, bool return_on_signal)
{
	for (;;) {
		if (predicate != NULL && predicate(context)) {
			return;
		}

		k_spinlock_key_t key = k_spin_lock(&queue->lock);
		if (queue->closed) {
			k_spin_unlock(&queue->lock, key);
			return;
		}
		if (queue->waiters == 0) {
			while (k_sem_take(&queue->semaphore, K_NO_WAIT) == 0) {
			}
		}
		uint64_t observed = queue->generation;
		queue->waiters++;
		k_spin_unlock(&queue->lock, key);

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
		if (atomic_get(&fh_wait_pause) != 0) {
			atomic_set(&fh_wait_entered, 1);
			k_sem_take(&fh_wait_release, K_FOREVER);
		}
#endif

		if (predicate != NULL && predicate(context)) {
			fh_waiter_remove(queue);
			return;
		}

		key = k_spin_lock(&queue->lock);
		bool closed = queue->closed;
		bool signaled = queue->generation != observed;
		if (closed || signaled) {
			queue->waiters--;
			k_spin_unlock(&queue->lock, key);
			if (closed || return_on_signal) {
				return;
			}
			continue;
		}
		k_spin_unlock(&queue->lock, key);

		fh_task_note_block();
		k_sem_take(&queue->semaphore, K_FOREVER);
		fh_task_note_wake();
		fh_waiter_remove(queue);
		if (return_on_signal) {
			return;
		}
	}
}

void fh_wait_queue_destroy(size_t handle)
{
	struct fh_wait_queue *queue = fh_queue(handle);
	if (queue == NULL) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&queue->lock);
	queue->closed = true;
	queue->generation++;
	unsigned int waiters = queue->waiters;
	k_spin_unlock(&queue->lock, key);
	for (unsigned int i = 0; i < waiters; ++i) {
		k_sem_give(&queue->semaphore);
	}
	for (;;) {
		key = k_spin_lock(&queue->lock);
		waiters = queue->waiters;
		k_spin_unlock(&queue->lock, key);
		if (waiters == 0) {
			break;
		}
		/* The main thread may outrank vCPU threads; block so they can drain. */
		k_sleep(K_MSEC(1));
	}
	key = k_spin_lock(&fh_queue_table_lock);
	queue->used = false;
	k_spin_unlock(&fh_queue_table_lock, key);
}

void fh_wait_queue_wait(size_t handle)
{
	struct fh_wait_queue *queue = fh_queue(handle);
	if (queue != NULL) {
		fh_wait_common(queue, NULL, NULL, true);
	}
}

void fh_wait_queue_wait_until(size_t handle, fh_predicate_fn predicate, void *context)
{
	struct fh_wait_queue *queue = fh_queue(handle);
	if (queue != NULL) {
		fh_wait_common(queue, predicate, context, false);
	}
}

static void fh_wake(size_t handle, bool all)
{
	struct fh_wait_queue *queue = fh_queue(handle);
	if (queue == NULL) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&queue->lock);
	queue->generation++;
	unsigned int count = all ? queue->waiters : MIN(queue->waiters, 1U);
	k_spin_unlock(&queue->lock, key);
	for (unsigned int i = 0; i < count; ++i) {
		k_sem_give(&queue->semaphore);
	}
}

void fh_wait_queue_wake_one(size_t handle)
{
	fh_wake(handle, false);
}

void fh_wait_queue_wake_all(size_t handle)
{
	fh_wake(handle, true);
}

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
void fh_test_wait_pause_enable(bool enabled)
{
	atomic_set(&fh_wait_pause, enabled ? 1 : 0);
	atomic_set(&fh_wait_entered, 0);
	while (k_sem_take(&fh_wait_release, K_NO_WAIT) == 0) {
	}
}

bool fh_test_wait_pause_entered(void)
{
	return atomic_get(&fh_wait_entered) != 0;
}

void fh_test_wait_pause_release(void)
{
	k_sem_give(&fh_wait_release);
}
#endif
