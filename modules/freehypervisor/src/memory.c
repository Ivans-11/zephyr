/* SPDX-License-Identifier: Apache-2.0 */
#include <freehypervisor/zephyr_adapter.h>

#include <zephyr/kernel.h>
#include <zephyr/kernel/internal/mm.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

extern int arch_page_phys_get(void *virt, uintptr_t *phys);

#define FH_PAGE_SIZE 4096UL
#define FH_POOL_SIZE ((size_t)CONFIG_FREEHYPERVISOR_POOL_MIB * 1024UL * 1024UL)
#define FH_POOL_PAGES (FH_POOL_SIZE / FH_PAGE_SIZE)
#define FH_FREE 0U
#define FH_ALLOCATED 1U
#define FH_FREEING 2U

static uint8_t fh_pool[FH_POOL_SIZE] __aligned(2 * 1024 * 1024);
static uint8_t fh_page_state[FH_POOL_PAGES];
static struct k_spinlock fh_pool_lock;
static uintptr_t fh_pool_pa;
static uint64_t fh_alloc_sequence;

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
static atomic_t fh_free_pause;
static atomic_t fh_free_entered;
K_SEM_DEFINE(fh_free_release, 0, 1);
#endif

static bool fh_range_is(uint8_t state, size_t first, size_t count)
{
	for (size_t i = first; i < first + count; ++i) {
		if (fh_page_state[i] != state) {
			return false;
		}
	}
	return true;
}

int fh_memory_init(void)
{
	uintptr_t first = 0;
	uintptr_t last = 0;
	int rc = arch_page_phys_get(fh_pool, &first);

	if (rc != 0) {
		printk("FH: pool VA->PA failed: %d\n", rc);
		return rc;
	}
	rc = arch_page_phys_get(&fh_pool[FH_POOL_SIZE - FH_PAGE_SIZE], &last);
	if (rc != 0 || last != first + FH_POOL_SIZE - FH_PAGE_SIZE) {
		printk("FH: pool is not physically contiguous first=%#lx last=%#lx rc=%d\n",
		       first, last, rc);
		return -EINVAL;
	}
	fh_pool_pa = first;
	memset(fh_page_state, FH_FREE, sizeof(fh_page_state));
	printk("FH: reserved pool PA [%#lx, %#lx) VA=%p pages=%zu\n",
	       fh_pool_pa, fh_pool_pa + FH_POOL_SIZE, fh_pool, (size_t)FH_POOL_PAGES);
	return 0;
}

uint64_t fh_alloc_frames(size_t count, size_t align_bytes)
{
	if (count == 0 || count > FH_POOL_PAGES) {
		return 0;
	}
	if (align_bytes < FH_PAGE_SIZE) {
		align_bytes = FH_PAGE_SIZE;
	}
	if ((align_bytes & (align_bytes - 1)) != 0) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&fh_pool_lock);
	for (size_t first = 0; first + count <= FH_POOL_PAGES; ++first) {
		uintptr_t candidate = fh_pool_pa + first * FH_PAGE_SIZE;
		if ((candidate % align_bytes) != 0) {
			continue;
		}
		if (!fh_range_is(FH_FREE, first, count)) {
			continue;
		}
		memset(&fh_page_state[first], FH_ALLOCATED, count);
		uint64_t sequence = ++fh_alloc_sequence;
		k_spin_unlock(&fh_pool_lock, key);
		memset(&fh_pool[first * FH_PAGE_SIZE], 0, count * FH_PAGE_SIZE);
		printk("FH: alloc seq=%llu PA=%#lx pages=%zu align_bytes=%zu\n",
		       sequence, candidate, count, align_bytes);
		return candidate;
	}
	k_spin_unlock(&fh_pool_lock, key);
	return 0;
}

void fh_free_frames(uint64_t paddr, size_t count)
{
	if (paddr < fh_pool_pa || count == 0 || ((paddr - fh_pool_pa) % FH_PAGE_SIZE) != 0) {
		printk("FH: invalid free PA=%#llx pages=%zu\n", paddr, count);
		return;
	}
	size_t first = (size_t)((paddr - fh_pool_pa) / FH_PAGE_SIZE);
	if (first + count > FH_POOL_PAGES) {
		printk("FH: out-of-range free PA=%#llx pages=%zu\n", paddr, count);
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&fh_pool_lock);
	if (!fh_range_is(FH_ALLOCATED, first, count)) {
		k_spin_unlock(&fh_pool_lock, key);
		printk("FH: ownership violation on free PA=%#llx pages=%zu\n", paddr, count);
		return;
	}
	memset(&fh_page_state[first], FH_FREEING, count);
	k_spin_unlock(&fh_pool_lock, key);

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
	if (atomic_get(&fh_free_pause) != 0) {
		atomic_set(&fh_free_entered, 1);
		k_sem_take(&fh_free_release, K_FOREVER);
	}
#endif

	memset(&fh_pool[first * FH_PAGE_SIZE], 0, count * FH_PAGE_SIZE);
	key = k_spin_lock(&fh_pool_lock);
	memset(&fh_page_state[first], FH_FREE, count);
	k_spin_unlock(&fh_pool_lock, key);
}

void *fh_phys_to_virt(uint64_t paddr)
{
	if (paddr < fh_pool_pa || paddr >= fh_pool_pa + FH_POOL_SIZE) {
		return NULL;
	}
	return &fh_pool[paddr - fh_pool_pa];
}

uint64_t fh_virt_to_phys(const void *vaddr)
{
	uintptr_t va = (uintptr_t)vaddr;
	uintptr_t base = (uintptr_t)fh_pool;
	if (va >= base && va < base + FH_POOL_SIZE) {
		return fh_pool_pa + va - base;
	}
	uintptr_t page = ROUND_DOWN(va, FH_PAGE_SIZE);
	uintptr_t pa = 0;
	if (arch_page_phys_get((void *)page, &pa) != 0) {
		return 0;
	}
	return pa + (va - page);
}

uint64_t fh_pool_base_paddr(void)
{
	return fh_pool_pa;
}

size_t fh_pool_page_count(void)
{
	return FH_POOL_PAGES;
}

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
void fh_test_free_pause_enable(bool enabled)
{
	atomic_set(&fh_free_pause, enabled ? 1 : 0);
	atomic_set(&fh_free_entered, 0);
	while (k_sem_take(&fh_free_release, K_NO_WAIT) == 0) {
	}
}

bool fh_test_free_pause_entered(void)
{
	return atomic_get(&fh_free_entered) != 0;
}

void fh_test_free_pause_release(void)
{
	k_sem_give(&fh_free_release);
}
#endif
