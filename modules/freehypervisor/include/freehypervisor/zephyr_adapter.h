/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FREEHYPERVISOR_ZEPHYR_ADAPTER_H
#define FREEHYPERVISOR_ZEPHYR_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*fh_predicate_fn)(void *context);
typedef void (*fh_entry_fn)(void *context);
typedef void (*fh_irq_fn)(size_t vector);

int fh_adapter_init(void);
void fh_console_write(const uint8_t *bytes, size_t length);
size_t fh_console_read(uint8_t *bytes, size_t length);
void fh_console_acquire(void);
void fh_console_release(void);
void *fh_rust_alloc(size_t size, size_t align);
void fh_rust_dealloc(void *ptr, size_t size, size_t align);
void fh_rust_abort(void) __attribute__((noreturn));

size_t fh_host_cpu_count(void);
size_t fh_current_cpu_id(void);
uint32_t fh_host_tsc_frequency_mhz(void);

#if defined(CONFIG_X86_64)
void fh_prepare_x86_virtualization(void);
#endif

uint64_t fh_alloc_frames(size_t count, size_t align_bytes);
void fh_free_frames(uint64_t paddr, size_t count);
void *fh_phys_to_virt(uint64_t paddr);
uint64_t fh_virt_to_phys(const void *vaddr);
uint64_t fh_pool_base_paddr(void);
size_t fh_pool_page_count(void);

size_t fh_task_spawn(const uint8_t *name, size_t name_len, size_t stack_size,
		     size_t cpu_mask, fh_entry_fn entry, void *context);
void fh_task_join(size_t handle);
size_t fh_task_current(void);
void fh_task_yield(void);
void fh_task_note_block(void);
void fh_task_note_wake(void);

size_t fh_wait_queue_create(void);
void fh_wait_queue_destroy(size_t queue);
void fh_wait_queue_wait(size_t queue);
void fh_wait_queue_wait_until(size_t queue, fh_predicate_fn predicate, void *context);
void fh_wait_queue_wake_one(size_t queue);
void fh_wait_queue_wake_all(size_t queue);

uint64_t fh_monotonic_nanos(void);
void fh_set_oneshot_timer(uint64_t deadline_ns);
void fh_time_shutdown(void);

bool fh_irq_handle(size_t vector);
bool fh_irq_register(size_t vector, fh_irq_fn handler);
int fh_hv_ipi_init(void);
int fh_hv_ipi_send(unsigned int cpu_id);
uint64_t fh_hv_ipi_count(void);
unsigned int fh_hv_ipi_last_cpu(void);

#ifdef CONFIG_FREEHYPERVISOR_CORE
int fh_rust_init(void);
void fh_rust_run(void);
void fh_rust_timer_expired(void);
#endif

#ifdef CONFIG_FREEHYPERVISOR_CONTRACT_TESTS
void fh_test_wait_pause_enable(bool enabled);
bool fh_test_wait_pause_entered(void);
void fh_test_wait_pause_release(void);
void fh_test_free_pause_enable(bool enabled);
bool fh_test_free_pause_entered(void);
void fh_test_free_pause_release(void);
void fh_test_timer_suppress_callback(bool suppress);
uint64_t fh_test_timer_expiry_count(void);
uint64_t fh_test_timer_last_fire_ns(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
