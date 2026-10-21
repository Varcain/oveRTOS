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

/* ── First-creation self-identity race ──────────────────────────────────
 * Several threads that start together, before any ove thread has ever run,
 * must each resolve ove_thread_get_self() to their own handle.  Regression:
 * the NuttX backend allocated its task-TLS index lazily from every child
 * with no lock, so children racing through the first allocation could each
 * take a slot; the shared index then pointed at only one of them and the
 * others read NULL.  The race exists only while that index is unset, so
 * this must be the first test in the suite (and the Thread suite runs
 * before any other suite that creates threads).  On a single core the
 * equal-priority children run that setup back to back, so this is a smoke
 * test there: it catches the bug only if a child is preempted inside the
 * allocation (SMP, or a yield injected to widen the window). */
#define SELF_RACE_N 4
OVE_TEST_STORAGE(ove_thread_storage_t, s_race_storage[SELF_RACE_N]);
OVE_TEST_STACK(s_race_stack0, 4096);
OVE_TEST_STACK(s_race_stack1, 4096);
OVE_TEST_STACK(s_race_stack2, 4096);
OVE_TEST_STACK(s_race_stack3, 4096);
OVE_TEST_STORAGE(ove_sem_storage_t, s_race_sem_storage);

static ove_sem_t s_race_go;
static atomic_int s_race_arrived;
static atomic_int s_race_done;
static atomic_intptr_t s_race_self[SELF_RACE_N];

static void entry_self_race(void *arg)
{
	intptr_t idx = (intptr_t)arg;
	atomic_fetch_add(&s_race_arrived, 1);
	/* Released together only once every child has started, so each reads
	 * its identity after all of them went through the TLS setup. */
	int rc = ove_sem_take(s_race_go, 2000ULL * 1000ULL * 1000ULL);
	(void)rc; /* a timeout shows up as a missing/late record, not a hang */
	atomic_store(&s_race_self[idx], (intptr_t)ove_thread_get_self());
	atomic_fetch_add(&s_race_done, 1);
}

static void test_get_self_concurrent_first_start(void **state)
{
	(void)state;
	void *stacks[SELF_RACE_N] = {s_race_stack0, s_race_stack1, s_race_stack2, s_race_stack3};
	static const char *const names[SELF_RACE_N] = {"race0", "race1", "race2", "race3"};
	ove_thread_t h[SELF_RACE_N] = {NULL};
	int made[SELF_RACE_N] = {0};
	int created = 0;

	atomic_store(&s_race_arrived, 0);
	atomic_store(&s_race_done, 0);
	OVE_TEST_ASSERT_OK(ove_test_sem_create(&s_race_go, &s_race_sem_storage, 0, SELF_RACE_N));
	/* Same priority as the runner: none starts until the runner blocks, so
	 * all of them go through their first TLS setup back to back. */
	for (int i = 0; i < SELF_RACE_N; i++) {
		if (ove_test_thread_run(&h[i], &s_race_storage[i], names[i], entry_self_race,
					(void *)(intptr_t)i, stacks[i], 4096) == OVE_OK) {
			made[i] = 1;
			created++;
		}
	}
	int arrived = wait_for_atomic(&s_race_arrived, created, 2000);
	for (int i = 0; i < created; i++)
		ove_sem_give(s_race_go);
	int done = wait_for_atomic(&s_race_done, created, 2000);

	int mismatches = 0;
	for (int i = 0; i < SELF_RACE_N; i++) {
		if (!made[i])
			continue;
		intptr_t seen = atomic_load(&s_race_self[i]);
		if (seen != (intptr_t)h[i]) {
			mismatches++;
			print_message("    race%d: ove_thread_get_self()=%p, handle=%p\n", i,
				      (void *)seen, (void *)h[i]);
		}
	}
	print_message("    %d/%d threads resolved their own handle\n", created - mismatches,
		      created);
	for (int i = 0; i < SELF_RACE_N; i++) {
		if (made[i])
			ove_test_thread_destroy(h[i]);
	}
	ove_test_sem_destroy(s_race_go);

	assert_int_equal(created, SELF_RACE_N);
	assert_true(arrived);
	assert_true(done);
	assert_int_equal(mismatches, 0);
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

static void entry_spin_flagged(void *arg)
{
	atomic_store(&g_flag, 1);
	entry_spin(arg);
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

/* A thread that outranks its creator can start running inside the create call; it must
 * already resolve itself there, or ove_thread_should_stop(ove_thread_get_self()) could
 * never become true for it.  The creator is a NORMAL-priority ove thread rather than the
 * runner, whose native priority differs per target (Zephyr runs it as main() at priority
 * 0, which outranks OVE_PRIO_HIGH, so a HIGH child created from it never preempts).  On a
 * preemptive engine the HIGH child therefore runs to completion inside create; the host
 * POSIX backend does not enforce priorities, so there only the identity is checked.
 * The child also queries its own state there: the handle's backend fields (FreeRTOS task,
 * NuttX pid) must already be in place, not only its identity. */
OVE_TEST_STORAGE(ove_thread_storage_t, s_spawner_storage);
OVE_TEST_STACK(s_spawner_stack, 4096);

static atomic_int s_child_ran;
static atomic_intptr_t s_child_self;
static atomic_intptr_t s_child_handle;
static atomic_int s_ran_at_return;
static atomic_int s_spawn_rc;
static atomic_int s_spawn_done;
static atomic_int s_child_state;

static void entry_child_self(void *arg)
{
	(void)arg;
	ove_thread_t self = ove_thread_get_self();
	atomic_store(&s_child_self, (intptr_t)self);
	atomic_store(&s_child_state, (int)ove_thread_get_state(self));
	atomic_store(&s_child_ran, 1);
}

static void entry_spawner(void *arg)
{
	(void)arg;
	ove_thread_t child = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	/* Start from never-used storage: earlier tests leave this block holding the
	 * same backend fields a correct init would write, which would mask a field
	 * the child reads before its creator's init has set it. */
	memset(&s_th_storage, 0, sizeof(s_th_storage));
	int rc = ove_thread_init(&child, &s_th_storage, "t_self", entry_child_self, NULL,
				 OVE_PRIO_HIGH, 4096, s_th_stack);
#else
	int rc = ove_thread_create(&child, "t_self", entry_child_self, NULL, OVE_PRIO_HIGH, 4096);
#endif
	atomic_store(&s_ran_at_return, atomic_load(&s_child_ran));
	atomic_store(&s_child_handle, (intptr_t)child);
	atomic_store(&s_spawn_rc, rc);
	atomic_store(&s_spawn_done, 1);
}

static void test_get_self_before_create_returns(void **state)
{
	(void)state;
	atomic_store(&s_child_ran, 0);
	atomic_store(&s_child_self, 0);
	atomic_store(&s_child_handle, 0);
	atomic_store(&s_ran_at_return, -1);
	atomic_store(&s_spawn_rc, OVE_OK);
	atomic_store(&s_spawn_done, 0);
	atomic_store(&s_child_state, OVE_THREAD_STATE_UNKNOWN);

	ove_thread_t spawner = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&spawner, &s_spawner_storage, "t_spawn",
					       entry_spawner, NULL, s_spawner_stack, 4096));
	int done = wait_for_atomic(&s_spawn_done, 1, 2000);
	int ran = wait_for_atomic(&s_child_ran, 1, 2000);
	ove_thread_t child = (ove_thread_t)atomic_load(&s_child_handle);
	intptr_t self = atomic_load(&s_child_self);
	int at_return = atomic_load(&s_ran_at_return);
	int child_state = atomic_load(&s_child_state);
	print_message("    child self=%p handle=%p, ran before create returned=%d, own state=%d\n",
		      (void *)self, (void *)child, at_return, child_state);
	if (child != NULL)
		ove_test_thread_destroy(child);
	ove_test_thread_destroy(spawner);

	assert_true(done);
	assert_int_equal(atomic_load(&s_spawn_rc), OVE_OK);
	assert_true(ran);
	assert_non_null(child);
	assert_int_equal(self, (intptr_t)child);
	/* Running, or ready where the engine keeps the current thread queued. */
	assert_true(child_state == OVE_THREAD_STATE_RUNNING ||
		    child_state == OVE_THREAD_STATE_READY);
#ifndef CONFIG_OVE_RTOS_POSIX
	assert_int_equal(at_return, 1);
#endif
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

/* NULL handles: what Thread::current() and get_self() hand back on a thread
 * oveRTOS did not create.  Every engine must reject them the same way; the
 * FreeRTOS backend used to dereference them. */
static void test_null_handle_queries(void **state)
{
	(void)state;
	struct ove_thread_stats st;

	ove_thread_suspend(NULL);
	ove_thread_resume(NULL);
	assert_int_equal(ove_thread_get_state(NULL), OVE_THREAD_STATE_UNKNOWN);
	assert_int_equal(ove_thread_get_runtime_stats(NULL, &st), OVE_ERR_INVALID_PARAM);
}

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
	}
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
		/* Must stay first: see test_get_self_concurrent_first_start. */
		cmocka_unit_test(test_get_self_concurrent_first_start),
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
		cmocka_unit_test(test_null_handle_queries),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_destroy_null),
#endif
		cmocka_unit_test_teardown(test_runtime_stats, teardown_stop_spin),
		cmocka_unit_test_teardown(test_thread_list_identity, teardown_stop_spin),
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
