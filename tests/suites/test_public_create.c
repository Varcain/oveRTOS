/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Smoke test for the public ove_*_create() API.
 *
 * Each case invokes the documented public create macro / function
 * directly — NOT the test-framework's ove_test_*_create wrapper.  In
 * zero-heap mode the macros expand to "({ static <storage>; static <buf>;
 * <init>; })" GNU statement-expressions; in heap mode they are real
 * functions.  The test-framework wrappers paper over both, which means
 * a backend-specific bug in the public macro (the kind that broke the
 * Zephyr zero-heap workqueue stack — see backends/zephyr/zephyr_workqueue.c)
 * will not be caught by tests that only call the wrapper.
 *
 * Each create call lives in its own function so the per-call-site
 * statics generated in zero-heap mode produce exactly one kernel
 * object per case.  Where the object hosts a thread (workqueue,
 * thread), the test also exercises a single submit / entry so the
 * stack and dispatch path are walked at least once — that is the
 * shape of the failure that would otherwise hide here.
 */

#include "../framework/ove_test.h"
#include <stdatomic.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static volatile int s_thread_ran;
static volatile int s_work_ran;
static ove_sem_t s_thread_done_sem;
static ove_sem_t s_work_done_sem;

static void thread_entry_signal(void *arg)
{
	(void)arg;
	s_thread_ran = 1;
	ove_sem_give(s_thread_done_sem);
}

static void work_handler_signal(ove_work_t work)
{
	(void)work;
	s_work_ran = 1;
	ove_sem_give(s_work_done_sem);
}

static void timer_cb_noop(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_public_create_mutex(void **state)
{
	(void)state;
	ove_mutex_t m = NULL;
	int rc = ove_mutex_create(&m);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(m);
	assert_int_equal(ove_mutex_lock(m, OVE_WAIT_FOREVER), OVE_OK);
	ove_mutex_unlock(m);
	ove_mutex_destroy(m);
}

static void test_public_create_recursive_mutex(void **state)
{
	(void)state;
	ove_mutex_t m = NULL;
	int rc = ove_recursive_mutex_create(&m);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(m);
	assert_int_equal(ove_recursive_mutex_lock(m, OVE_WAIT_FOREVER), OVE_OK);
	assert_int_equal(ove_recursive_mutex_lock(m, OVE_WAIT_FOREVER), OVE_OK);
	ove_recursive_mutex_unlock(m);
	ove_recursive_mutex_unlock(m);
	ove_recursive_mutex_destroy(m);
}

static void test_public_create_sem(void **state)
{
	(void)state;
	ove_sem_t s = NULL;
	int rc = ove_sem_create(&s, 1, 4);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(s);
	assert_int_equal(ove_sem_take(s, 0), OVE_OK);
	ove_sem_give(s);
	ove_sem_destroy(s);
}

static void test_public_create_event(void **state)
{
	(void)state;
	ove_event_t e = NULL;
	int rc = ove_event_create(&e);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(e);
	ove_event_destroy(e);
}

static void test_public_create_condvar(void **state)
{
	(void)state;
	ove_condvar_t cv = NULL;
	int rc = ove_condvar_create(&cv);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(cv);
	ove_condvar_destroy(cv);
}

static void test_public_create_queue(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	int rc = ove_queue_create(&q, sizeof(int), 4);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(q);
	int in = 0xCAFE;
	int out = 0;
	assert_int_equal(ove_queue_send(q, &in, 0), OVE_OK);
	assert_int_equal(ove_queue_receive(q, &out, 0), OVE_OK);
	assert_int_equal(out, 0xCAFE);
	ove_queue_destroy(q);
}

static void test_public_create_timer(void **state)
{
	(void)state;
	ove_timer_t t = NULL;
	int rc = ove_timer_create(&t, timer_cb_noop, NULL, 1000, 1);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(t);
	ove_timer_destroy(t);
}

static void test_public_create_eventgroup(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	int rc = ove_eventgroup_create(&eg);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(eg);
	(void)ove_eventgroup_set_bits(eg, 0x1);
	ove_eventgroup_destroy(eg);
}

static void test_public_create_stream(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	int rc = ove_stream_create(&s, 64, 1);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(s);
	const uint8_t tx[] = {1, 2, 3, 4};
	uint8_t rx[8] = {0};
	size_t written = 0, read = 0;
	assert_int_equal(ove_stream_send(s, tx, sizeof(tx), 0, &written), OVE_OK);
	assert_int_equal(written, sizeof(tx));
	assert_int_equal(ove_stream_receive(s, rx, sizeof(rx), 0, &read), OVE_OK);
	assert_int_equal(read, sizeof(tx));
	ove_stream_destroy(s);
}

/*
 * Thread: spawn an entry that signals a sem, wait for it, then destroy.
 * Verifies that the public ove_thread_create() macro produces a thread
 * whose stack is correctly aligned/sized — a misaligned static stack
 * would fault on first context switch.  The wait-for-completion is the
 * critical part: it forces the new thread to actually run, not just be
 * created.
 */
static void test_public_create_thread(void **state)
{
	(void)state;
	assert_int_equal(ove_sem_create(&s_thread_done_sem, 0, 1), OVE_OK);
	s_thread_ran = 0;

	ove_thread_t h = NULL;
	const struct ove_thread_desc desc = {
		.name = "pub_th",
		.entry = thread_entry_signal,
		.arg = NULL,
		.priority = OVE_PRIO_NORMAL,
	};
	int rc = ove_thread_create(&h, 1024, &desc);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(h);

	assert_int_equal(ove_sem_take(s_thread_done_sem, 1000), OVE_OK);
	assert_int_equal(s_thread_ran, 1);

	ove_thread_destroy(h);
	ove_sem_destroy(s_thread_done_sem);
}

/*
 * Workqueue: create, submit one work item, wait for handler completion.
 * This is the case that originally hid the Zephyr zero-heap regression —
 * the macro was producing a stack with no MPU/guard reservation, and
 * submit_execute walked off the bottom.  Submitting + waiting forces
 * the workqueue thread to actually run the dispatch loop.
 */
static void test_public_create_workqueue(void **state)
{
	(void)state;
	assert_int_equal(ove_sem_create(&s_work_done_sem, 0, 1), OVE_OK);
	s_work_ran = 0;

	ove_workqueue_t wq = NULL;
	int rc = ove_workqueue_create(&wq, "pub_wq", OVE_PRIO_NORMAL, 1024);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(wq);

	static ove_work_storage_t work_storage;
	ove_work_t w = NULL;
	assert_int_equal(ove_work_init_static(&w, &work_storage,
					      work_handler_signal), OVE_OK);
	assert_non_null(w);

	assert_int_equal(ove_work_submit(wq, w), OVE_OK);
	assert_int_equal(ove_sem_take(s_work_done_sem, 1000), OVE_OK);
	assert_int_equal(s_work_ran, 1);

	ove_workqueue_destroy(wq);
	ove_sem_destroy(s_work_done_sem);
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Heap-only: ove_work_init self-allocates the work item.  In zero-heap
 * mode work items always come from caller-supplied storage via
 * ove_work_init_static (already exercised by test_public_create_workqueue).
 */
static void test_public_create_work(void **state)
{
	(void)state;
	ove_work_t w = NULL;
	int rc = ove_work_init(&w, work_handler_signal);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(w);
	ove_work_free(w);
}
#endif

#ifndef CONFIG_OVE_ZERO_HEAP
/* Heap-only: sim watchdog backend (POSIX) has no static-storage path.
 * Mirrors the gating in test_watchdog.c. */
static void test_public_create_watchdog(void **state)
{
	(void)state;
	ove_watchdog_t wd = NULL;
	int rc = ove_watchdog_create(&wd, 5000);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(wd);
	ove_watchdog_destroy(wd);
}
#endif

#if defined(CONFIG_OVE_ZERO_HEAP) && defined(CONFIG_OVE_RTOS_ZEPHYR)
/*
 * Zephyr zero-heap proof: CONFIG_HEAP_MEM_POOL_SIZE=0 means Zephyr
 * doesn't instantiate _system_heap at all.  Reference it as a weak
 * extern — link succeeds either way, but in a properly-configured
 * zero-heap build the symbol's address is NULL because no heap was
 * defined.  A nonzero address would mean the kernel heap snuck back
 * in (e.g. someone enabled CONFIG_HEAP_MEM_POOL_SIZE>0 in a board
 * overlay).
 */
extern struct sys_heap _system_heap __attribute__((weak));

static void test_public_create_no_kernel_heap(void **state)
{
	(void)state;
	assert_null(&_system_heap);
}
#endif

#ifdef CONFIG_OVE_ZERO_HEAP
/*
 * Zero-heap proof — exercise the actual trap.  ove_heap_lock() flips
 * an atomic flag that the --wrap=malloc trampoline (in
 * backends/common/ove_heap_lock.c) checks.  In normal operation a
 * post-lock malloc DEBUGASSERTs and aborts; for tests the test-mode
 * hooks swap the abort for "return NULL + increment counter" so we
 * can verify the trap fires without tearing down the suite.
 *
 * The wrap functions are portable across NuttX / FreeRTOS / Zephyr.
 * Calling __wrap_malloc directly tests the lock-check + trap-counter
 * logic regardless of whether raw malloc() is intercepted at link
 * time (which depends on each backend's --wrap LDFLAGS plumbing).
 */
#include <stdlib.h>

extern void ove_heap_lock(void);
extern void ove_heap_lock_test_begin(void);
extern int  ove_heap_lock_test_end(void);
extern void *__wrap_malloc(size_t n);
extern void  __wrap_free(void *p);

static void test_public_create_heap_lock_callable(void **state)
{
	(void)state;
	/* Plain ove_heap_lock() is idempotent — calling twice is OK. */
	ove_heap_lock();
	ove_heap_lock();
	/* Drop the lock so subsequent suites still work. */
	(void)ove_heap_lock_test_end();
}

static void test_public_create_heap_lock_traps(void **state)
{
	(void)state;

	/* Pre-condition: with no lock engaged, the wrapper forwards to
	 * the real allocator and returns a usable pointer. */
	void *pre = __wrap_malloc(16);
	assert_non_null(pre);
	__wrap_free(pre);

	/* Engage the lock in test mode (returns NULL + bumps a counter
	 * instead of DEBUGASSERTing — DEBUGASSERT mid-suite would abort
	 * the test binary).  Now the same call must be denied. */
	ove_heap_lock_test_begin();
	void *post = __wrap_malloc(16);
	int  trap_count = ove_heap_lock_test_end();
	assert_null(post);
	assert_int_equal(trap_count, 1);
}
#endif

/* ── runner ──────────────────────────────────────────────────────────── */

int test_public_create_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_public_create_mutex),
		cmocka_unit_test(test_public_create_recursive_mutex),
		cmocka_unit_test(test_public_create_sem),
		cmocka_unit_test(test_public_create_event),
		cmocka_unit_test(test_public_create_condvar),
		cmocka_unit_test(test_public_create_queue),
		cmocka_unit_test(test_public_create_timer),
		cmocka_unit_test(test_public_create_eventgroup),
		cmocka_unit_test(test_public_create_stream),
		cmocka_unit_test(test_public_create_thread),
		cmocka_unit_test(test_public_create_workqueue),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_public_create_work),
		cmocka_unit_test(test_public_create_watchdog),
#endif
#if defined(CONFIG_OVE_ZERO_HEAP) && defined(CONFIG_OVE_RTOS_ZEPHYR)
		cmocka_unit_test(test_public_create_no_kernel_heap),
#endif
#ifdef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_public_create_heap_lock_callable),
		cmocka_unit_test(test_public_create_heap_lock_traps),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
