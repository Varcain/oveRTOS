/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

static atomic_int g_flag;
static atomic_intptr_t g_arg_val;
static atomic_intptr_t g_self_seen;
static atomic_int g_keep_running;
static atomic_int g_progress;

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

static void entry_record_self(void *arg)
{
	(void)arg;
	atomic_store(&g_self_seen, (intptr_t)ove_thread_get_self());
	atomic_store(&g_flag, 1);
}

static void entry_spin(void *arg)
{
	(void)arg;
	while (atomic_load(&g_keep_running)) {
		test_msleep(1);
	}
}

static void entry_spin_flagged(void *arg)
{
	atomic_store(&g_flag, 1);
	entry_spin(arg);
}

/* Keeps making observable progress until asked to stop; used to prove that
 * suspend actually parks the thread. */
static void entry_count_progress(void *arg)
{
	(void)arg;
	atomic_store(&g_flag, 1);
	while (atomic_load(&g_keep_running)) {
		atomic_fetch_add(&g_progress, 1);
		test_msleep(1);
	}
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

/* A thread that outranks its creator can start running inside the create call; it must
 * already resolve itself there, or ove_thread_should_stop(ove_thread_get_self()) could
 * never become true for it. */
static void test_get_self_before_create_returns(void **state)
{
	(void)state;
	atomic_store(&g_flag, 0);
	atomic_store(&g_self_seen, 0);
	ove_thread_t h = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_thread_init(&h, &s_th_storage, "t_self", entry_record_self, NULL,
					   OVE_PRIO_HIGH, 4096, s_th_stack));
#else
	OVE_TEST_ASSERT_OK(
		ove_thread_create(&h, "t_self", entry_record_self, NULL, OVE_PRIO_HIGH, 4096));
#endif
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	assert_int_equal(atomic_load(&g_self_seen), (intptr_t)h);
	ove_test_thread_destroy(h);
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
	while (st != OVE_THREAD_STATE_TERMINATED) {
		(void)ove_time_get_us(&st_now);
		if (st_now >= st_start + 1000000u)
			break;
		test_msleep(1);
		st = ove_thread_get_state(h);
	}
	assert_int_equal(st, OVE_THREAD_STATE_TERMINATED);
#if defined(CONFIG_OVE_RTOS_POSIX) && !defined(CONFIG_OVE_ZERO_HEAP)
	/* POSIX publishes a race-free final coloration snapshot rather than
	 * scanning another pthread's live stack. */
	size_t final_headroom = 0;
	assert_int_equal(ove_thread_get_stack_headroom(h, &final_headroom), OVE_OK);
	assert_true(final_headroom <= 512u * 1024u);
#endif
	ove_test_thread_destroy(h);
}

/* 10. capability-aware stack headroom and compatibility query */
static void test_stack_usage(void **state)
{
	(void)state;
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(
		ove_test_thread_run(&h, &s_th_storage, "t10", entry_spin, NULL, s_th_stack, 4096));
	test_msleep(10);
	size_t headroom = SIZE_MAX;
	int rc = ove_thread_get_stack_headroom(h, &headroom);
	assert_true(rc == OVE_OK || rc == OVE_ERR_NOT_SUPPORTED);
	if (rc == OVE_ERR_NOT_SUPPORTED)
		assert_int_equal(headroom, 0);
	else
		assert_int_equal(ove_thread_get_stack_usage(h), headroom);
	assert_int_equal(ove_thread_get_stack_headroom(h, NULL), OVE_ERR_INVALID_PARAM);
	headroom = SIZE_MAX;
	assert_int_equal(ove_thread_get_stack_headroom(NULL, &headroom), OVE_ERR_INVALID_PARAM);
	assert_int_equal(headroom, 0);
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
	atomic_store(&g_keep_running, 1);
	atomic_store(&g_progress, 0);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "t14", entry_count_progress, NULL,
					       s_th_stack, 4096));
	/* Wait for the thread to start (it sets g_flag=1 on entry). */
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	test_msleep(20);
	int before = atomic_load(&g_progress);
	assert_true(before > 0);

	ove_thread_suspend(h);
	test_msleep(20);
	int mid = atomic_load(&g_progress);
	test_msleep(20);
	int mid2 = atomic_load(&g_progress);
	/* Parked: a sane sleep loop makes no progress across 20 ms. */
	assert_int_equal(mid, mid2);

	ove_thread_resume(h);
	test_msleep(20);
	assert_true(atomic_load(&g_progress) > mid2);

	atomic_store(&g_keep_running, 0);
	ove_test_thread_destroy(h);
}

/* 14b. a suspend right after a resume, before the lower-priority target
 * ran again, is not lost: the target is either SUSPENDED and makes no
 * progress, or not SUSPENDED and keeps running. */
static void test_suspend_resume_back_to_back(void **state)
{
	(void)state;
	atomic_store(&g_flag, 0);
	atomic_store(&g_keep_running, 1);
	atomic_store(&g_progress, 0);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "t14b", entry_count_progress,
					       NULL, s_th_stack, 4096));
	ove_thread_set_priority(h, OVE_PRIO_LOW);
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	test_msleep(20);

	int bad = 0;
	for (int round = 0; round < 5; round++) {
		ove_thread_suspend(h);
		test_msleep(10);
		ove_thread_resume(h);
		ove_thread_suspend(h);
		ove_thread_state_t st = ove_thread_get_state(h);
		int a = atomic_load(&g_progress);
		test_msleep(50);
		int b = atomic_load(&g_progress);
		if (st == OVE_THREAD_STATE_SUSPENDED ? a != b : a == b)
			bad++;
		ove_thread_resume(h);
		test_msleep(20);
	}

	atomic_store(&g_keep_running, 0);
	ove_thread_resume(h);
	ove_test_thread_destroy(h);
	assert_int_equal(bad, 0);
}

/* The state ove_thread_list reports for the thread named @p name: -1 if it
 * is not listed, -2 if the backend cannot list threads. */
static int listed_state(const char *name)
{
	struct ove_thread_info info[32];
	size_t count = 0;
	if (ove_thread_list(info, 32, &count) == OVE_ERR_NOT_SUPPORTED)
		return -2;
	for (size_t i = 0; i < count; i++) {
		if (strcmp(info[i].name, name) == 0)
			return (int)info[i].state;
	}
	return -1;
}

/* 14c. destroy returns once the thread is gone, not only its entry
 * function: the caller may reuse the storage and the stack right away
 * (zero-heap).  The thread runs below the caller, so on its way out it is
 * preempted as soon as it wakes the caller; a destroy that only waits for
 * that wake-up returns while the thread still runs on its stack, and the
 * thread is still listed. */
static void test_destroy_waits_for_exit(void **state)
{
	(void)state;
	atomic_store(&g_flag, 0);
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "t14c", entry_count_progress,
					       NULL, s_th_stack, 4096));
	ove_thread_set_priority(h, OVE_PRIO_LOW);
	assert_true(wait_for_atomic(&g_flag, 1, 1000));

	atomic_store(&g_keep_running, 0);
	ove_test_thread_destroy(h);
	int listed = listed_state("t14c");
	assert_true(listed == -1 || listed == -2 || listed == OVE_THREAD_STATE_TERMINATED);
}

/* Wait up to @p timeout_ms for @p h to reach @p expected; returns whether
 * it did. */
static int wait_for_state(ove_thread_t h, ove_thread_state_t expected, uint32_t timeout_ms)
{
	uint64_t start = 0, now = 0;
	(void)ove_time_get_us(&start);
	uint64_t deadline = start + (uint64_t)timeout_ms * 1000u;
	while (ove_thread_get_state(h) != expected) {
		(void)ove_time_get_us(&now);
		if (now >= deadline)
			return ove_thread_get_state(h) == expected;
		test_msleep(1);
	}
	return 1;
}

/* 14d. ove_thread_list reports a parked thread as SUSPENDED, as
 * ove_thread_get_state() does, and stops after the resume.  NuttX has no
 * suspended task state, so its list takes the suspension from the thread
 * itself, with or without the state tracker. */
static void test_suspend_listed_state(void **state)
{
	(void)state;
	atomic_store(&g_flag, 0);
	atomic_store(&g_keep_running, 1);
	atomic_store(&g_progress, 0);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "t14d", entry_count_progress,
					       NULL, s_th_stack, 4096));
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	test_msleep(20);

	/* The suspension can land asynchronously (POSIX delivers a signal):
	 * wait for it, or a park that comes after the resume below would
	 * outlive it.  Without it the thread is left alone and the test fails:
	 * a late park would hang any join. */
	ove_thread_suspend(h);
	if (!wait_for_state(h, OVE_THREAD_STATE_SUSPENDED, 1000)) {
		atomic_store(&g_keep_running, 0);
		fail_msg("t14d did not reach SUSPENDED");
	}
	int parked = listed_state("t14d");
	ove_thread_resume(h);
	test_msleep(20);
	int resumed = listed_state("t14d");

	atomic_store(&g_keep_running, 0);
	ove_test_thread_destroy(h);

	if (parked == -2)
		return; /* no thread enumeration in this build */
	assert_int_equal(parked, OVE_THREAD_STATE_SUSPENDED);
	assert_int_not_equal(resumed, -1);
	assert_int_not_equal(resumed, OVE_THREAD_STATE_SUSPENDED);
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

/* 17. enumeration exposes a stable native identity. Personality ownership is
 * derived by the personality adapter rather than stored in the generic API. */
static void test_thread_list_identity(void **state)
{
	(void)state;
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&h, &s_th_storage, "tenum", entry_spin, NULL,
					       s_th_stack, 4096));
	test_msleep(20);

	struct ove_thread_info info[32];
	size_t count = 0;
	int rc = ove_thread_list(info, 32, &count);

	/* Check while the spinner is alive (names may point into its TCB), but
	 * assert only after stopping it: a failed assert must not leave it
	 * running on storage the next test reuses. */
	const uint32_t known_fields =
		OVE_THREAD_INFO_VALID_STACK_USED | OVE_THREAD_INFO_VALID_STACK_SIZE |
		OVE_THREAD_INFO_VALID_CPU_PERCENT | OVE_THREAD_INFO_VALID_STATE_TIMES;
	bool fields_ok = true;
	bool found = false;
	bool identity_ok = false;
	bool state_times_ok = false;
	if (rc != OVE_ERR_NOT_SUPPORTED) {
		for (size_t i = 0; i < count; i++) {
			if (info[i].valid_fields & ~known_fields)
				fields_ok = false;
			if (!ove_thread_info_has(&info[i], OVE_THREAD_INFO_VALID_STACK_USED) &&
			    info[i].stack_used != 0)
				fields_ok = false;
			if (!ove_thread_info_has(&info[i], OVE_THREAD_INFO_VALID_STACK_SIZE) &&
			    info[i].stack_size != 0)
				fields_ok = false;
			if (!ove_thread_info_has(&info[i], OVE_THREAD_INFO_VALID_CPU_PERCENT) &&
			    info[i].cpu_percent_x100 != 0)
				fields_ok = false;
			if (strcmp(info[i].name, "tenum") == 0) {
				identity_ok = info[i].identity != 0;
#ifdef CONFIG_OVE_THREAD_STATE_STATS
				/* An ove thread has a state tracker, so it must report
				 * all four state times, not only running time. */
				state_times_ok = ove_thread_info_has(
					&info[i], OVE_THREAD_INFO_VALID_STATE_TIMES);
#else
				state_times_ok = true;
#endif
				found = true;
				break;
			}
		}
	}

	atomic_store(&g_keep_running, 0);
	test_msleep(20);
	ove_test_thread_destroy(h);

	assert_true(rc == OVE_OK || rc == OVE_ERR_QUEUE_FULL || rc == OVE_ERR_NOT_SUPPORTED);
	if (rc != OVE_ERR_NOT_SUPPORTED) {
		assert_true(fields_ok);
		assert_true(found);
		assert_true(identity_ok);
		assert_true(state_times_ok);
	}
}

/* Run "tgen" on the shared storage, then report its generation and trace
 * id from ove_thread_list() (both 0 if it is not listed). */
static void run_and_list_tgen(uint32_t *generation, uintptr_t *trace_id)
{
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	OVE_TEST_ASSERT_OK(
		ove_test_thread_run(&h, &s_th_storage, "tgen", entry_spin, NULL, s_th_stack, 4096));
	test_msleep(20);

	struct ove_thread_info info[32];
	size_t count = 0;
	*generation = 0;
	*trace_id = 0;
	if (ove_thread_list(info, 32, &count) != OVE_ERR_NOT_SUPPORTED) {
		for (size_t i = 0; i < count; i++) {
			if (strcmp(info[i].name, "tgen") == 0) {
				*generation = info[i].generation;
				*trace_id = info[i].trace_id;
				break;
			}
		}
	}

	atomic_store(&g_keep_running, 0);
	test_msleep(20);
	ove_test_thread_destroy(h);
}

/* 17b. a new thread on reused storage gets a new lane generation, so the
 * trace can tell it from the previous thread; a backend that does not
 * report generations lists 0. */
static void test_thread_list_generation(void **state)
{
	(void)state;
	uint32_t gen1, gen2;
	uintptr_t tid1, tid2;
	run_and_list_tgen(&gen1, &tid1);
	run_and_list_tgen(&gen2, &tid2);
	if (gen1 == 0 && gen2 == 0)
		return;
	assert_true(gen1 != 0 && gen2 != 0);
	assert_true(gen1 != gen2);
	assert_true(tid1 != 0 && tid2 != 0);
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

/* 17. OVE_PRIO_ABOVE / OVE_PRIO_BELOW step along the scale and clamp at its ends */
static void test_priority_scale_helpers(void **state)
{
	(void)state;
	_Static_assert(OVE_PRIO_ABOVE(OVE_PRIO_NORMAL, 1) > OVE_PRIO_NORMAL &&
			       OVE_PRIO_ABOVE(OVE_PRIO_NORMAL, 1) < OVE_PRIO_ABOVE_NORMAL,
		       "one step above an anchor stays below the next anchor");
	assert_int_equal(OVE_PRIO_ABOVE(OVE_PRIO_NORMAL, 1), OVE_PRIO_NORMAL + 1);
	assert_int_equal(OVE_PRIO_ABOVE(OVE_PRIO_HIGH, 100), OVE_PRIO_MAX);
	assert_int_equal(OVE_PRIO_ABOVE(OVE_PRIO_CRITICAL, 1), OVE_PRIO_MAX);
	assert_int_equal(OVE_PRIO_BELOW(OVE_PRIO_NORMAL, 4), OVE_PRIO_BELOW_NORMAL);
	assert_int_equal(OVE_PRIO_BELOW(OVE_PRIO_LOW, 10), OVE_PRIO_IDLE);
}

/* 18. a priority above OVE_PRIO_MAX is rejected */
static void test_create_priority_out_of_range(void **state)
{
	(void)state;
	ove_thread_t h = NULL;
	const ove_prio_t bad = (ove_prio_t)(OVE_PRIO_MAX + 1);
#ifdef CONFIG_OVE_ZERO_HEAP
	int rc = ove_thread_init(&h, &s_th_storage, "tbad", entry_set_flag, NULL, bad, 4096,
				 s_th_stack);
#else
	int rc = ove_thread_create(&h, "tbad", entry_set_flag, NULL, bad, 4096);
#endif
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	assert_null(h);
}

/* 19. a thread runs at a priority between two anchors, and can be moved to another one;
 * set_priority ignores a priority above the scale */
static void test_priority_between_anchors(void **state)
{
	(void)state;
	atomic_store(&g_flag, 0);
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	const ove_prio_t between = OVE_PRIO_ABOVE(OVE_PRIO_NORMAL, 1);
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_thread_init(&h, &s_th_storage, "tmid", entry_spin_flagged, NULL,
					   between, 4096, s_th_stack));
#else
	OVE_TEST_ASSERT_OK(ove_thread_create(&h, "tmid", entry_spin_flagged, NULL, between, 4096));
#endif
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	ove_thread_set_priority(h, OVE_PRIO_BELOW(OVE_PRIO_HIGH, 2));
	ove_thread_set_priority(h, (ove_prio_t)(OVE_PRIO_MAX + 1));
	atomic_store(&g_keep_running, 0);
	test_msleep(20);
	ove_test_thread_destroy(h);
}

/* The priority ove_thread_list reports for the thread named @p name, or -1. */
static int listed_priority(const char *name)
{
	struct ove_thread_info info[32];
	size_t count = 0;
	if (ove_thread_list(info, 32, &count) != OVE_OK)
		return -1;
	for (size_t i = 0; i < count; i++) {
		if (strcmp(info[i].name, name) == 0)
			return info[i].priority;
	}
	return -1;
}

/* 20. ove_thread_list reports a thread's ove priority, after set_priority too */
static void test_thread_list_priority(void **state)
{
	(void)state;
	struct ove_thread_info probe[1];
	size_t n = 0;
	if (ove_thread_list(probe, 1, &n) == OVE_ERR_NOT_SUPPORTED)
		skip();
	atomic_store(&g_flag, 0);
	atomic_store(&g_keep_running, 1);
	ove_thread_t h = NULL;
	const ove_prio_t created = OVE_PRIO_ABOVE(OVE_PRIO_NORMAL, 1);
	const ove_prio_t moved = OVE_PRIO_BELOW(OVE_PRIO_HIGH, 2);
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_thread_init(&h, &s_th_storage, "tprio", entry_spin_flagged, NULL,
					   created, 4096, s_th_stack));
#else
	OVE_TEST_ASSERT_OK(ove_thread_create(&h, "tprio", entry_spin_flagged, NULL, created, 4096));
#endif
	assert_true(wait_for_atomic(&g_flag, 1, 1000));
	int at_create = listed_priority("tprio");
	ove_thread_set_priority(h, moved);
	int after_set = listed_priority("tprio");
	atomic_store(&g_keep_running, 0);
	test_msleep(20);
	ove_test_thread_destroy(h);
	assert_int_equal(at_create, created);
	assert_int_equal(after_set, moved);
}

/* errno belongs to each thread: values set on either side of a context
 * switch must not leak into the other thread. */
static atomic_int g_errno_main_set;
static atomic_int g_errno_seen;

static void entry_errno(void *arg)
{
	(void)arg;
	errno = 1111;
	atomic_store(&g_flag, 1);
	while (!atomic_load(&g_errno_main_set))
		test_msleep(1);
	atomic_store(&g_errno_seen, errno);
	atomic_store(&g_flag, 2);
}

static void test_errno_per_thread(void **state)
{
	(void)state;
	atomic_store(&g_flag, 0);
	atomic_store(&g_errno_main_set, 0);
	atomic_store(&g_errno_seen, 0);
	errno = 0;
	ove_thread_t h = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_thread_init(&h, &s_th_storage, "terrno", entry_errno, NULL,
					   OVE_PRIO_NORMAL, 4096, s_th_stack));
#else
	OVE_TEST_ASSERT_OK(
		ove_thread_create(&h, "terrno", entry_errno, NULL, OVE_PRIO_NORMAL, 4096));
#endif
	int started = wait_for_atomic(&g_flag, 1, 1000);
	int main_errno_after_start = errno;
	errno = 2222;
	atomic_store(&g_errno_main_set, 1);
	int finished = wait_for_atomic(&g_flag, 2, 1000);
	int main_errno = errno;
	ove_test_thread_destroy(h);
	assert_true(started);
	assert_true(finished);
	assert_int_equal(main_errno_after_start, 0);
	assert_int_equal(main_errno, 2222);
	assert_int_equal(atomic_load(&g_errno_seen), 1111);
}

int test_thread_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_create_destroy),
		cmocka_unit_test(test_entry_arg),
		cmocka_unit_test(test_sleep_duration),
		cmocka_unit_test(test_yield),
		cmocka_unit_test(test_start_scheduler),
		cmocka_unit_test(test_get_self),
		cmocka_unit_test(test_get_self_before_create_returns),
		cmocka_unit_test_teardown(test_set_priority, teardown_stop_spin),
		cmocka_unit_test_teardown(test_get_state_running, teardown_stop_spin),
		cmocka_unit_test(test_get_state_terminated),
		cmocka_unit_test_teardown(test_stack_usage, teardown_stop_spin),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_create_null_handle),
		cmocka_unit_test(test_create_null_entry),
#endif
		cmocka_unit_test(test_suspend_resume),
		cmocka_unit_test(test_suspend_resume_back_to_back),
		cmocka_unit_test(test_destroy_waits_for_exit),
		cmocka_unit_test(test_suspend_listed_state),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_destroy_null),
#endif
		cmocka_unit_test_teardown(test_runtime_stats, teardown_stop_spin),
		cmocka_unit_test_teardown(test_thread_list_identity, teardown_stop_spin),
		cmocka_unit_test_teardown(test_thread_list_generation, teardown_stop_spin),
		cmocka_unit_test(test_priority_scale_helpers),
		cmocka_unit_test(test_create_priority_out_of_range),
		cmocka_unit_test_teardown(test_priority_between_anchors, teardown_stop_spin),
		cmocka_unit_test_teardown(test_thread_list_priority, teardown_stop_spin),
		cmocka_unit_test(test_errno_per_thread),
#ifdef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_create_misaligned_stack),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
