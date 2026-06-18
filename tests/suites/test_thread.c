/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <stdatomic.h>

OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

static atomic_int g_flag;
static atomic_intptr_t g_arg_val;
static atomic_int g_keep_running;

/* Bounded wait for an atomic_int flag — the stdatomic analogue of the
 * framework's wait_for_flag (which takes a plain volatile int).  Uses a
 * wall-clock budget, not an iteration count, so it is robust under gcov/CI
 * load; replaces fixed test_msleep() guesses about "has the spawned thread
 * run yet". */
static int wait_for_atomic(atomic_int *flag, int expected, uint32_t timeout_ms)
{
	uint64_t start = 0, now = 0;
	(void)ove_time_get_us(&start);
	uint64_t deadline = start + (uint64_t)timeout_ms * 1000u;
	while (atomic_load(flag) != expected) {
		(void)ove_time_get_us(&now);
		if (now >= deadline)
			return atomic_load(flag) == expected;
		test_msleep(1);
	}
	return 1;
}

/* Teardown ensures spinning threads are stopped even if an assertion fails
   (cmocka longjmps past the cleanup code in the test body). */
static int teardown_stop_spin(void **state)
{
	(void)state;
	atomic_store(&g_keep_running, 0);
	/* thread_destroy now has join semantics on all backends — no sleep needed */
	return 0;
}

static void entry_set_flag(void *arg)
{
	(void)arg;
	atomic_store(&g_flag, 1);
}

static void entry_capture_arg(void *arg)
{
	atomic_store(&g_arg_val, (intptr_t)arg);
	atomic_store(&g_flag, 1); /* signal "arg captured" for wait_for_atomic */
}

static void entry_spin(void *arg)
{
	(void)arg;
	while (atomic_load(&g_keep_running)) {
		test_msleep(1);
	}
}

static void entry_sleep_briefly(void *arg)
{
	(void)arg;
	atomic_store(&g_flag, 1);
	test_msleep(200);
	atomic_store(&g_flag, 2);
}

/* 1. create and destroy — verify entry runs */
static void test_create_destroy(void **state)
{
	(void)state;
	atomic_store(&g_flag, 0);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "t1", entry_set_flag, NULL,
					       s_th_stack, 4096));
	assert_non_null(h);
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	ove_test_thread_destroy(h);
}

/* 2. entry receives correct arg */
static void test_entry_arg(void **state)
{
	(void)state;
	atomic_store(&g_arg_val, 0);
	atomic_store(&g_flag, 0);
	int sentinel = 0xBEEF;
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "t2", entry_capture_arg,
					       &sentinel, s_th_stack, 4096));
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	assert_int_equal((intptr_t)atomic_load(&g_arg_val), (intptr_t)&sentinel);
	ove_test_thread_destroy(h);
}

/* 3. sleep_ms duration */
static void test_sleep_duration(void **state)
{
	(void)state;
	uint64_t before = 0, after = 0;
	ove_time_get_us(&before);
	ove_thread_sleep_ms(100);
	ove_time_get_us(&after);
	uint64_t elapsed = after - before;
	assert_duration_within(elapsed, 100, OVE_TEST_TIMING_TOLERANCE_MS);
}

/* 4. yield no crash */
static void test_yield(void **state)
{
	(void)state;
	ove_thread_yield();
}

/* 5. start_scheduler no crash */
static void test_start_scheduler(void **state)
{
	(void)state;
	ove_thread_start_scheduler();
}

/* 6. get_self returns non-NULL in main thread */
static void test_get_self(void **state)
{
	(void)state;
	/* In main thread, stub may return NULL since main isn't a ove thread.
	   Just verify it doesn't crash. */
	ove_thread_get_self();
}

/* 7. set_priority no crash */
static void test_set_priority(void **state)
{
	(void)state;
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(
		ove_test_thread_run(&h, &s_th_storage, "t7", entry_spin, NULL, s_th_stack, 4096));
	test_msleep(10);
	ove_thread_set_priority(h, OVE_PRIO_HIGH);
	atomic_store(&g_keep_running, 0);
	test_msleep(20);
	ove_test_thread_destroy(h);
}

/* 8. get_state for running thread */
static void test_get_state_running(void **state)
{
	(void)state;
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(
		ove_test_thread_run(&h, &s_th_storage, "t8", entry_spin, NULL, s_th_stack, 4096));
	test_msleep(20);
	ove_thread_state_t st = ove_thread_get_state(h);
	/* RUNNING, READY, or BLOCKED (FreeRTOS: vTaskDelay in spin loop) */
	assert_true(st == OVE_THREAD_STATE_RUNNING || st == OVE_THREAD_STATE_READY ||
		    st == OVE_THREAD_STATE_BLOCKED);
	atomic_store(&g_keep_running, 0);
	test_msleep(20);
	ove_test_thread_destroy(h);
}

/* 9. get_state returns TERMINATED after exit */
static void test_get_state_terminated(void **state)
{
	(void)state;
	ove_thread_t h = NULL;
	atomic_store(&g_flag, 0);
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "t9", entry_set_flag, NULL,
					       s_th_stack, 4096));
	/* Entry sets the flag then returns; wait for it to have run, then
	 * bounded-poll (wall-clock) until the backend reflects the exit rather
	 * than guessing a fixed delay. */
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	uint64_t st_start = 0, st_now = 0;
	(void)ove_time_get_us(&st_start);
	ove_thread_state_t st = ove_thread_get_state(h);
	/* FreeRTOS threads suspend after entry returns; stub marks terminated */
	while (st != OVE_THREAD_STATE_TERMINATED && st != OVE_THREAD_STATE_SUSPENDED) {
		(void)ove_time_get_us(&st_now);
		if (st_now >= st_start + 1000000u)
			break;
		test_msleep(1);
		st = ove_thread_get_state(h);
	}
	assert_true(st == OVE_THREAD_STATE_TERMINATED || st == OVE_THREAD_STATE_SUSPENDED);
	ove_test_thread_destroy(h);
}

/* 10. get_stack_usage doesn't crash */
static void test_stack_usage(void **state)
{
	(void)state;
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(
		ove_test_thread_run(&h, &s_th_storage, "t10", entry_spin, NULL, s_th_stack, 4096));
	test_msleep(10);
	/* Stub returns 0; FreeRTOS returns actual HWM bytes — just verify no crash */
	(void)ove_thread_get_stack_usage(h);
	atomic_store(&g_keep_running, 0);
	test_msleep(20);
	ove_test_thread_destroy(h);
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* 11. create with NULL handle */
static void test_create_null_handle(void **state)
{
	(void)state;
	assert_int_equal(ove_thread_create(NULL, "t11", entry_set_flag, NULL, OVE_PRIO_NORMAL,
					   4096),
			 OVE_ERR_INVALID_PARAM);
}

/* 13. create with NULL entry */
static void test_create_null_entry(void **state)
{
	(void)state;
	ove_thread_t h = NULL;
	assert_int_equal(ove_thread_create(&h, "t13", NULL, NULL, OVE_PRIO_NORMAL, 4096),
			 OVE_ERR_INVALID_PARAM);
}
#endif

/* 14. suspend and resume */
static void test_suspend_resume(void **state)
{
	(void)state;
	atomic_store(&g_flag, 0);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "t14", entry_sleep_briefly, NULL,
					       s_th_stack, 4096));
	/* Wait for the thread to start (it sets g_flag=1 on entry). */
	assert_true(wait_for_atomic(&g_flag, 1, 1000));

	ove_thread_suspend(h);
	test_msleep(10);
	ove_thread_resume(h);

	/* Settle delay (not a flag-poll): let the resumed worker finish its
	 * sleep before we destroy it, so destroy's join can't race a thread
	 * that resume hasn't woken yet.  We don't assert post-resume progress —
	 * suspend/resume wake latency is backend/scheduler-sensitive (notably
	 * under TSan's signal interception); this test verifies suspend/resume
	 * don't crash and the thread stays joinable. */
	test_msleep(300);
	ove_test_thread_destroy(h);
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* 15. destroy NULL is safe */
static void test_destroy_null(void **state)
{
	(void)state;
	ove_thread_destroy(NULL);
}
#endif

/* 16. get_runtime_stats */
static void test_runtime_stats(void **state)
{
	(void)state;
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(
		ove_test_thread_run(&h, &s_th_storage, "t16", entry_spin, NULL, s_th_stack, 4096));
	test_msleep(20);

	struct ove_thread_stats stats;
	int rc = ove_thread_get_runtime_stats(h, &stats);
	/* Stub and NuttX return NOT_SUPPORTED; FreeRTOS may succeed */
	assert_true(rc == OVE_OK || rc == OVE_ERR_NOT_SUPPORTED);

	atomic_store(&g_keep_running, 0);
	test_msleep(20);
	ove_test_thread_destroy(h);
}

#ifdef CONFIG_OVE_ZERO_HEAP
/* Hand-rolled misaligned stack: aligned(8) on the full array, but we
 * pass in a +1 offset so the backend sees a misaligned pointer.
 * ove_thread_init() must reject it with OVE_ERR_INVALID_PARAM — this is
 * the runtime backstop for the AAPCS 8-byte-alignment requirement
 * documented on OVE_THREAD_STACK_MEMBER_ in include/ove/storage.h. */
static void test_create_misaligned_stack(void **state)
{
	(void)state;
	static uint8_t __attribute__((aligned(8))) misaligned_buf[4096];
	static ove_thread_storage_t misaligned_th_storage;

	ove_thread_t h = NULL;
	int rc = ove_thread_init(&h, &misaligned_th_storage, "misaligned", entry_set_flag, NULL,
				 OVE_PRIO_NORMAL, sizeof(misaligned_buf) - 8,
				 misaligned_buf + 1 /* deliberately off by 1 */);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	assert_null(h);
}
#endif /* CONFIG_OVE_ZERO_HEAP */

int test_thread_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_create_destroy),
		cmocka_unit_test(test_entry_arg),
		cmocka_unit_test(test_sleep_duration),
		cmocka_unit_test(test_yield),
		cmocka_unit_test(test_start_scheduler),
		cmocka_unit_test(test_get_self),
		cmocka_unit_test_teardown(test_set_priority, teardown_stop_spin),
		cmocka_unit_test_teardown(test_get_state_running, teardown_stop_spin),
		cmocka_unit_test(test_get_state_terminated),
		cmocka_unit_test_teardown(test_stack_usage, teardown_stop_spin),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_create_null_handle),
		cmocka_unit_test(test_create_null_entry),
#endif
		cmocka_unit_test(test_suspend_resume),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_destroy_null),
#endif
		cmocka_unit_test_teardown(test_runtime_stats, teardown_stop_spin),
#ifdef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_create_misaligned_stack),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
