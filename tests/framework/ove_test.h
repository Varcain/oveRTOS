/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_TEST_H
#define OVE_TEST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include "ove/ove.h"

#include "ove_test_common.h"

/* ── Static storage macros ────────────────────────────────────────────
 * Always declare the storage variables so helpers compile in both modes.
 * In heap mode the helpers simply ignore the storage parameter.
 *
 * OVE_TEST_STACK forwards to OVE_THREAD_STACK_DEFINE_ so backends that
 * need special alignment/sectioning (Zephyr's K_THREAD_STACK_DEFINE for
 * MPU + guard region) get it. A raw uint8_t[] hid the zero-heap workqueue
 * stack regression on Zephyr; routing through the public helper keeps the
 * test framework honest about what production code requires.
 */
#define OVE_TEST_STORAGE(type, name) static type name __attribute__((unused))
#define OVE_TEST_STACK(name, size) OVE_THREAD_STACK_DEFINE_STATIC_(name, size)

/*
 * Portable sleep for test code.
 * Uses ove_thread_sleep_ms() so it works correctly with all backends
 * (POSIX stub, FreeRTOS POSIX port, etc.).
 * For the stub backend, usleep() also works, but the FreeRTOS POSIX port
 * requires FreeRTOS API calls for sleeping inside tasks.
 */
static inline void test_msleep(uint32_t ms)
{
	ove_thread_sleep_ms(ms);
}

/*
 * Wait until *flag == expected or the timeout expires.
 *
 * Returns 1 if the flag reached the expected value within the deadline,
 * 0 on timeout. Uses monotonic wall-clock (ove_time_get_us) for a real
 * timeout rather than an iteration-count budget. Sleeps 1 ms between
 * polls so a quick signal lands within ~one tick.
 *
 * Prefer this over hand-rolled `for (i=0; i<N && !flag; i++) msleep(1)`
 * loops: the budget is in wall-clock time, the exit path is explicit,
 * and the final flag value is returned so the caller can assert on it.
 */
/* Set a flag the test main thread is polling via wait_for_flag.  Use
 * this in worker threads / callbacks so the C11-atomic store pairs
 * with wait_for_flag's atomic-acquire load — keeps TSan green. */
#define TEST_FLAG_SET(flag_lvalue, value) \
	__atomic_store_n(&(flag_lvalue), (value), __ATOMIC_RELEASE)

/* P1-4: ove_mutex_lock / ove_recursive_mutex_lock / ove_condvar_wait
 * are OVE_NODISCARD; tests that call them with OVE_WAIT_FOREVER must
 * still consume the return value to silence -Wunused-result. These
 * wrappers assert OK via cmocka's assert_int_equal — unexpected lock
 * failure fails the test cleanly rather than silently proceeding. */
#define OVE_TEST_LOCK(mtx) assert_int_equal(ove_mutex_lock((mtx), OVE_WAIT_FOREVER), OVE_OK)
#define OVE_TEST_RECURSIVE_LOCK(mtx) \
	assert_int_equal(ove_recursive_mutex_lock((mtx), OVE_WAIT_FOREVER), OVE_OK)
#define OVE_TEST_CONDVAR_WAIT(cv, mtx) \
	assert_int_equal(ove_condvar_wait((cv), (mtx), OVE_WAIT_FOREVER), OVE_OK)

/* Read flag with C11 acquire ordering so TSan / language-rules see this
 * as an atomic load.  Writers must pair via TEST_FLAG_SET (above). */
static inline int wait_for_flag(volatile int *flag, int expected, uint32_t timeout_ms)
{
	uint64_t start_us = 0, now_us = 0;
	(void)ove_time_get_us(&start_us);
	uint64_t deadline_us = start_us + (uint64_t)timeout_ms * 1000u;

	while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) != expected) {
		(void)ove_time_get_us(&now_us);
		if (now_us >= deadline_us) {
			return (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == expected);
		}
		test_msleep(1);
	}
	return 1;
}

/*
 * Join-and-destroy helper for threads launched by tests.
 *
 * Waits for the thread to exit (bounded by `timeout_ms`) then destroys
 * its storage. Returns OVE_OK on clean join, negative on timeout or
 * destroy error. Tests should call this instead of relying on sleeps
 * to estimate when a thread has finished.
 *
 * Note: `ove_thread_destroy` / `ove_thread_deinit` in the current
 * backends block until the entry function returns, so destroy already
 * provides the join. The `timeout_ms` argument is reserved for a future
 * explicit `ove_thread_join()` API — today it's advisory.
 */
static inline int ove_test_thread_join_destroy(ove_thread_t th, uint32_t timeout_ms)
{
	(void)timeout_ms;
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_thread_deinit(th);
#else
	return ove_thread_destroy(th);
#endif
}

/* ── Object creation helpers ─────────────────────────────────────────── */

static inline int ove_test_mutex_create(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_mutex_init(mtx, storage);
#else
	(void)storage;
	return ove_mutex_create(mtx);
#endif
}

static inline int ove_test_recursive_mutex_create(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_recursive_mutex_init(mtx, storage);
#else
	(void)storage;
	return ove_recursive_mutex_create(mtx);
#endif
}

static inline int ove_test_sem_create(ove_sem_t *sem, ove_sem_storage_t *storage,
				      unsigned int initial, unsigned int max)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_sem_init(sem, storage, initial, max);
#else
	(void)storage;
	return ove_sem_create(sem, initial, max);
#endif
}

static inline int ove_test_event_create(ove_event_t *evt, ove_event_storage_t *storage)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_event_init(evt, storage);
#else
	(void)storage;
	return ove_event_create(evt);
#endif
}

static inline int ove_test_condvar_create(ove_condvar_t *cv, ove_condvar_storage_t *storage)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_condvar_init(cv, storage);
#else
	(void)storage;
	return ove_condvar_create(cv);
#endif
}

static inline int ove_test_queue_create(ove_queue_t *q, ove_queue_storage_t *storage, void *buffer,
					size_t item_size, unsigned int max_items)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_queue_init(q, storage, buffer, item_size, max_items);
#else
	(void)storage;
	(void)buffer;
	return ove_queue_create(q, item_size, max_items);
#endif
}

static inline int ove_test_timer_create(ove_timer_t *timer, ove_timer_storage_t *storage,
					ove_timer_fn callback, void *user_data, uint32_t period_ms,
					int one_shot)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_timer_init(timer, storage, callback, user_data, period_ms, one_shot);
#else
	(void)storage;
	return ove_timer_create(timer, callback, user_data, period_ms, one_shot);
#endif
}

/* Nanosecond-period variant — picks ove_timer_init_ns / ove_timer_create_ns
 * based on heap policy.  Only meaningful when CONFIG_OVE_ASYNC is set
 * (the new ns symbols are gated alongside the async substrate). */
static inline int ove_test_timer_create_ns(ove_timer_t *timer, ove_timer_storage_t *storage,
					   ove_timer_fn callback, void *user_data,
					   uint64_t period_ns, int one_shot)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_timer_init_ns(timer, storage, callback, user_data, period_ns, one_shot);
#else
	(void)storage;
	return ove_timer_create_ns(timer, callback, user_data, period_ns, one_shot);
#endif
}

static inline int ove_test_eventgroup_create(ove_eventgroup_t *eg,
					     ove_eventgroup_storage_t *storage)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_eventgroup_init(eg, storage);
#else
	(void)storage;
	return ove_eventgroup_create(eg);
#endif
}

static inline int ove_test_stream_create(ove_stream_t *stream, ove_stream_storage_t *storage,
					 void *buffer, size_t size, size_t trigger)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_stream_init(stream, storage, buffer, size, trigger);
#else
	(void)storage;
	(void)buffer;
	return ove_stream_create(stream, size, trigger);
#endif
}

static inline int ove_test_workqueue_create(ove_workqueue_t *wq, ove_workqueue_storage_t *storage,
					    const char *name, ove_prio_t priority,
					    size_t stack_size, void *stack)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_workqueue_init(wq, storage, name, priority, stack_size, stack);
#else
	(void)storage;
	(void)stack;
	return ove_workqueue_create(wq, name, priority, stack_size);
#endif
}

static inline int ove_test_watchdog_create(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage,
					   uint32_t timeout_ms)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_watchdog_init(wdt, storage, timeout_ms);
#else
	(void)storage;
	return ove_watchdog_create(wdt, timeout_ms);
#endif
}

/* ── Object destruction helpers ──────────────────────────────────────── */

static inline void ove_test_mutex_destroy(ove_mutex_t mtx)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mutex_deinit(mtx);
#else
	ove_mutex_destroy(mtx);
#endif
}

static inline void ove_test_recursive_mutex_destroy(ove_mutex_t mtx)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	/* recursive_mutex uses same deinit as mutex (no-op on FreeRTOS) */
	ove_mutex_deinit(mtx);
#else
	ove_recursive_mutex_destroy(mtx);
#endif
}

static inline void ove_test_sem_destroy(ove_sem_t sem)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_sem_deinit(sem);
#else
	ove_sem_destroy(sem);
#endif
}

static inline void ove_test_event_destroy(ove_event_t evt)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_event_deinit(evt);
#else
	ove_event_destroy(evt);
#endif
}

static inline void ove_test_condvar_destroy(ove_condvar_t cv)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_condvar_deinit(cv);
#else
	ove_condvar_destroy(cv);
#endif
}

static inline void ove_test_queue_destroy(ove_queue_t q)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_queue_deinit(q);
#else
	ove_queue_destroy(q);
#endif
}

static inline void ove_test_timer_destroy(ove_timer_t timer)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_timer_deinit(timer);
#else
	ove_timer_destroy(timer);
#endif
}

static inline void ove_test_eventgroup_destroy(ove_eventgroup_t eg)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_eventgroup_deinit(eg);
#else
	ove_eventgroup_destroy(eg);
#endif
}

static inline void ove_test_stream_destroy(ove_stream_t stream)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_stream_deinit(stream);
#else
	ove_stream_destroy(stream);
#endif
}

static inline void ove_test_workqueue_destroy(ove_workqueue_t wq)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_workqueue_deinit(wq);
#else
	ove_workqueue_destroy(wq);
#endif
}

static inline void ove_test_watchdog_destroy(ove_watchdog_t wdt)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_watchdog_deinit(wdt);
#else
	ove_watchdog_destroy(wdt);
#endif
}

/* ── Thread helper ───────────────────────────────────────────────────── */

static inline int ove_test_thread_run(ove_thread_t *th, ove_thread_storage_t *storage,
				      const char *name, ove_thread_fn entry, void *arg,
				      void *stack, size_t stack_size)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_thread_init(th, storage, name, entry, arg, OVE_PRIO_NORMAL, stack_size, stack);
#else
	(void)storage;
	(void)stack;
	return ove_thread_create(th, name, entry, arg, OVE_PRIO_NORMAL, stack_size);
#endif
}

static inline int ove_test_thread_destroy(ove_thread_t th)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return ove_thread_deinit(th);
#else
	return ove_thread_destroy(th);
#endif
}

/* Test suite runner declarations */
int test_thread_run(void);
int test_thread_stop_run(void);
int test_thread_stop_isolation_run(void);
int test_sync_mutex_run(void);
int test_sync_sem_run(void);
int test_sync_event_run(void);
int test_sync_condvar_run(void);
int test_sync_recursive_run(void);
int test_queue_run(void);
int test_timer_run(void);
int test_time_run(void);
int test_timeout_ns_run(void);
int test_deadline_until_run(void);
int test_eventgroup_run(void);
int test_workqueue_run(void);
int test_stream_run(void);
int test_public_create_run(void);
int test_console_run(void);
int test_watchdog_run(void);
int test_nvs_run(void);
int test_shell_run(void);
int test_audio_run(void);
int test_bsp_run(void);
int test_board_run(void);
int test_gpio_run(void);
int test_led_run(void);
int test_fs_run(void);
int test_lvgl_run(void);
int test_static_define_run(void);
int test_init_no_alloc_run(void);
int test_app_run(void);
int test_async_run(void);
int test_infer_run(void);
int test_net_mqtt_run(void);
int test_net_httpd_run(void);
int test_net_sntp_run(void);
int test_net_loopback_run(void);
int test_i2c_run(void);
int test_spi_run(void);
int test_uart_run(void);
int test_pm_run(void);
int test_storage_bounds_run(void);
int test_renode_stm32_obs_run(void);
int test_renode_stm32_periph_run(void);
int test_renode_stm32_net_run(void);
int test_hw_stm32f746_run(void);

#endif /* OVE_TEST_H */
