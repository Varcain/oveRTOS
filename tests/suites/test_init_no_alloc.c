/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Phase 5 / storage hygiene: every primitive's _init path must accept
 * caller-provided storage and make ZERO internal heap allocations.
 *
 * This is the contract the binding-side allocator approach depends on
 * (Zig allocates ove_*_storage_t from std.mem.Allocator, hands the
 * pointer to _init).  If a backend's _init silently mallocs, that
 * guarantee breaks at runtime in a way that's hard to catch by
 * inspection.
 *
 * The mechanism: backends/common/ove_heap_lock.c wraps malloc/calloc/
 * realloc/free via the linker's --wrap=.  In test mode, every wrapped
 * call returns NULL and bumps an atomic trap counter.  We bracket each
 * _init call between ove_heap_lock_test_begin / _end and assert the
 * trap count is zero AND the _init succeeded (proving the path
 * genuinely doesn't need the heap, not just that it tolerates a NULL
 * return).
 */

#include "../framework/ove_test.h"

#include <ove/eventgroup.h>
#include <ove/queue.h>
#include <ove/stream.h>
#include <ove/sync.h>
#include <ove/thread.h>
#include <ove/timer.h>
#include <ove/watchdog.h>
#include <ove/workqueue.h>

/* Heap-lock test-mode APIs are defined in backends/common/ove_heap_lock.c
 * but only when CONFIG_OVE_ZERO_HEAP is set (that's the file's gate).
 * The trap itself only catches mallocs when the backend's heap-lock
 * wraps are engaged at link time (-Wl,--wrap=malloc etc).  This is true
 * for FreeRTOS-QEMU and Zephyr-QEMU builds (via their {freertos,zephyr}
 * _heap_lock.c wrap layer); the POSIX stub build does NOT engage --wrap,
 * so on stub the trap is a no-op and this becomes a smoke test (proves
 * each _init path returns OVE_OK with caller-provided storage).
 *
 * In heap mode the trap symbols don't exist, so we stub them locally
 * — the test still serves as a smoke test there. */
#ifdef CONFIG_OVE_ZERO_HEAP
extern void ove_heap_lock_test_begin(void);
extern int ove_heap_lock_test_end(void);
#else
static inline void ove_heap_lock_test_begin(void)
{
}
static inline int ove_heap_lock_test_end(void)
{
	return 0;
}
#endif

/* NuttX's task-create path always goes through kmm_zalloc for the TCB +
 * task_group_s (group_allocate); task_create_with_stack honors the caller
 * stack but still touches the kernel mm region
 * for setup that the backend can't bypass without a deeper rewrite.
 * Backend comment in backends/nuttx/nuttx_thread.c documents this as a
 * known gap; "use FreeRTOS or Zephyr for strict zero-heap thread/
 * workqueue init" is the current guidance.  Until the long-term backend
 * fix lands (project task #18), thread + workqueue init-no-alloc tests
 * skip the trap assertion on NuttX and stay as smoke tests (proving
 * _init still returns OVE_OK with caller-provided storage). */
#if defined(CONFIG_OVE_ZERO_HEAP) && defined(CONFIG_OVE_RTOS_NUTTX)
#define OVE_NO_ALLOC_THREAD_TRAP_KNOWN_GAP 1
#endif

/* Wraps an _init call between begin/end, asserts both no-trap and OK.
 * The `rc` slot lets the caller use the rc afterward (deinit needs
 * the handle to have been initialised). */
#define TRACE_INIT(rc_var, init_expr)                        \
	do {                                                 \
		ove_heap_lock_test_begin();                  \
		(rc_var) = (init_expr);                      \
		const int _traps = ove_heap_lock_test_end(); \
		assert_int_equal(_traps, 0);                 \
		assert_int_equal((rc_var), OVE_OK);          \
	} while (0)

/* Same as TRACE_INIT but bypasses the trap layer entirely on backends
 * with a documented gap (currently NuttX thread + workqueue — see
 * OVE_NO_ALLOC_THREAD_TRAP_KNOWN_GAP above).  We can't just skip the
 * trap-count check: in test mode the wrap returns NULL, which makes
 * the underlying task_create / kmm_zalloc fail, which makes _init
 * return OVE_ERR_NO_MEMORY rather than OVE_OK.  So on the gap backends
 * we call _init without engaging test mode at all — keeping this as
 * a smoke test (_init returns OVE_OK with caller-provided storage)
 * without claiming the strict no-alloc guarantee. */
#ifdef OVE_NO_ALLOC_THREAD_TRAP_KNOWN_GAP
#define TRACE_INIT_KNOWN_GAP(rc_var, init_expr)     \
	do {                                        \
		(rc_var) = (init_expr);             \
		assert_int_equal((rc_var), OVE_OK); \
	} while (0)
#else
#define TRACE_INIT_KNOWN_GAP(rc_var, init_expr) TRACE_INIT(rc_var, init_expr)
#endif

/* ── Sync primitives ────────────────────────────────────────────────── */

static void test_mutex_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_mutex_storage_t, storage);
	ove_mutex_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_mutex_init(&h, &storage));
	assert_non_null(h);
	ove_mutex_deinit(h);
}

static void test_recursive_mutex_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_mutex_storage_t, storage);
	ove_mutex_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_recursive_mutex_init(&h, &storage));
	assert_non_null(h);
	ove_recursive_mutex_deinit(h);
}

static void test_sem_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_sem_storage_t, storage);
	ove_sem_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_sem_init(&h, &storage, 0, 10));
	assert_non_null(h);
	ove_sem_deinit(h);
}

static void test_event_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_event_storage_t, storage);
	ove_event_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_event_init(&h, &storage));
	assert_non_null(h);
	ove_event_deinit(h);
}

static void test_condvar_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_condvar_storage_t, storage);
	ove_condvar_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_condvar_init(&h, &storage));
	assert_non_null(h);
	ove_condvar_deinit(h);
}

/* ── EventGroup ─────────────────────────────────────────────────────── */

static void test_eventgroup_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_eventgroup_storage_t, storage);
	ove_eventgroup_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_eventgroup_init(&h, &storage));
	assert_non_null(h);
	ove_eventgroup_deinit(h);
}

/* ── Queue ──────────────────────────────────────────────────────────── */

static void test_queue_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_queue_storage_t, storage);
	static uint8_t buffer[sizeof(int) * 8];
	ove_queue_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_queue_init(&h, &storage, buffer, sizeof(int), 8));
	assert_non_null(h);
	ove_queue_deinit(h);
}

/* ── Stream ─────────────────────────────────────────────────────────── */

static void test_stream_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_stream_storage_t, storage);
	static uint8_t buffer[64];
	ove_stream_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_stream_init(&h, &storage, buffer, sizeof(buffer), 1));
	assert_non_null(h);
	ove_stream_deinit(h);
}

/* ── Timer ──────────────────────────────────────────────────────────── */

static void noop_timer_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
}

static void test_timer_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_timer_storage_t, storage);
	ove_timer_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_timer_init(&h, &storage, noop_timer_cb, NULL, 1000, 1));
	assert_non_null(h);
	ove_timer_deinit(h);
}

/* ── Workqueue + Work ───────────────────────────────────────────────── */

OVE_TEST_STACK(s_wq_stack, 2048);

static void test_workqueue_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_workqueue_storage_t, storage);
	ove_workqueue_t h = NULL;
	int rc;
	TRACE_INIT_KNOWN_GAP(rc, ove_workqueue_init(&h, &storage, "wq_noalloc", OVE_PRIO_NORMAL,
						    sizeof(s_wq_stack), s_wq_stack));
	assert_non_null(h);
	ove_workqueue_deinit(h);
}

static void noop_work_handler(ove_work_t work)
{
	(void)work;
}

static void test_work_init_static_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_work_storage_t, storage);
	ove_work_t w = NULL;
	int rc;
	TRACE_INIT(rc, ove_work_init_static(&w, &storage, noop_work_handler));
	assert_non_null(w);
	ove_work_deinit(w);
}

/* ── Thread ─────────────────────────────────────────────────────────── */

OVE_TEST_STACK(s_th_stack, 2048);

static volatile int s_th_ran;
static volatile uintptr_t s_th_local_addr;

static void quick_thread_entry(void *arg)
{
	(void)arg;
	uintptr_t local_marker = 0;
	__atomic_store_n(&s_th_local_addr, (uintptr_t)&local_marker, __ATOMIC_RELEASE);
	TEST_FLAG_SET(s_th_ran, 1);
}

static void test_thread_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_thread_storage_t, storage);
	ove_thread_t h = NULL;
	int rc;
	s_th_ran = 0;
	s_th_local_addr = 0;
	TRACE_INIT_KNOWN_GAP(rc,
			     ove_thread_init(&h, &storage, "th_noalloc", quick_thread_entry, NULL,
					     OVE_PRIO_NORMAL, sizeof(s_th_stack), s_th_stack));
	assert_non_null(h);
#if defined(CONFIG_OVE_RTOS_NUTTX) && !defined(CONFIG_ARCH_SIM)
	/* NuttX used to silently ignore this buffer and allocate another stack.
	 * Prove the worker's actual stack frame lives in the supplied storage.
	 * NuttX sim intentionally substitutes an enlarged host stack when
	 * CONFIG_SIM_STACKSIZE_ADJUSTMENT is non-zero. */
	assert_true(wait_for_flag(&s_th_ran, 1, 1000));
	uintptr_t local_addr = __atomic_load_n(&s_th_local_addr, __ATOMIC_ACQUIRE);
	uintptr_t stack_begin = (uintptr_t)s_th_stack;
	assert_true(local_addr >= stack_begin);
	assert_true(local_addr < stack_begin + sizeof(s_th_stack));
#endif
	/* Cleanly join the worker before exiting. */
	ove_thread_request_stop(h);
	(void)ove_thread_deinit(h);
}

/* ── Watchdog ───────────────────────────────────────────────────────── */

static void test_watchdog_init_no_alloc(void **state)
{
	(void)state;
	OVE_TEST_STORAGE(ove_watchdog_storage_t, storage);
	ove_watchdog_t h = NULL;
	int rc;
	TRACE_INIT(rc, ove_watchdog_init(&h, &storage, 5000));
	assert_non_null(h);
	ove_watchdog_deinit(h);
}

/* ── Runner ─────────────────────────────────────────────────────────── */

int test_init_no_alloc_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_mutex_init_no_alloc),
		cmocka_unit_test(test_recursive_mutex_init_no_alloc),
		cmocka_unit_test(test_sem_init_no_alloc),
		cmocka_unit_test(test_event_init_no_alloc),
		cmocka_unit_test(test_condvar_init_no_alloc),
		cmocka_unit_test(test_eventgroup_init_no_alloc),
		cmocka_unit_test(test_queue_init_no_alloc),
		cmocka_unit_test(test_stream_init_no_alloc),
		cmocka_unit_test(test_timer_init_no_alloc),
		cmocka_unit_test(test_workqueue_init_no_alloc),
		cmocka_unit_test(test_work_init_static_no_alloc),
		cmocka_unit_test(test_thread_init_no_alloc),
		cmocka_unit_test(test_watchdog_init_no_alloc),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
