/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Coverage for the deadline-based _until variants added in Phase 3.
 * Each shim is a static inline wrapper computing `deadline - now`
 * (clamped to 0, with OVE_WAIT_FOREVER preserved verbatim) and
 * dispatching to its duration sibling — these tests exercise the
 * boundary conditions (past, near-future, sentinel, wraparound).
 *
 * Timing notes: all "elapsed must be at least X" assertions use
 * X >= 100 ms with one tick of slop, so QEMU's 1–10 ms tick
 * resolution doesn't cause false negatives.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_mutex_storage_t, s_mtx_storage);
OVE_TEST_STORAGE(ove_sem_storage_t, s_sem_storage);
OVE_TEST_STORAGE(ove_event_storage_t, s_evt_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage_b);
OVE_TEST_STACK(s_th_stack, 4096);
OVE_TEST_STACK(s_th_stack_b, 4096);

/* ── ove_time_deadline_to_timeout_ns helper itself ──────────────────── */

static void test_helper_past_deadline_returns_zero(void **state)
{
	(void)state;
	uint64_t now = ove_time_now_steady_ns();
	/* A deadline 1 ms in the past clamps to 0. */
	uint64_t past = now > OVE_MS(1) ? now - OVE_MS(1) : 0;
	assert_int_equal(ove_time_deadline_to_timeout_ns(past), 0);
}

static void test_helper_wait_forever_passthrough(void **state)
{
	(void)state;
	assert_true(ove_time_deadline_to_timeout_ns(OVE_WAIT_FOREVER) == OVE_WAIT_FOREVER);
}

static void test_helper_future_deadline_returns_remaining(void **state)
{
	(void)state;
	uint64_t now = ove_time_now_steady_ns();
	uint64_t deadline = now + OVE_MS(100);
	uint64_t remaining = ove_time_deadline_to_timeout_ns(deadline);
	/* Some clock jitter is allowed between the two now() reads. */
	assert_true(remaining > OVE_MS(50));
	assert_true(remaining <= OVE_MS(100));
}

/* ── _until: deadline in the past ───────────────────────────────────── */

static void test_sem_take_until_past_deadline(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);
	uint64_t past = ove_time_now_steady_ns();
	test_msleep(5);
	uint64_t before = ove_time_now_steady_ns();
	assert_int_equal(ove_sem_take_until(sem, past), OVE_ERR_TIMEOUT);
	uint64_t elapsed = ove_time_now_steady_ns() - before;
	/* Past deadline -> immediate return (no block). */
	assert_true(elapsed < OVE_MS(50));
	ove_test_sem_destroy(sem);
}

static void test_event_wait_until_past_deadline(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	ove_test_event_create(&evt, &s_evt_storage);
	uint64_t past = ove_time_now_steady_ns();
	test_msleep(5);
	assert_int_equal(ove_event_wait_until(evt, past), OVE_ERR_TIMEOUT);
	ove_test_event_destroy(evt);
}

/* ── _until: near-future deadline blocks until deadline ─────────────── */

static void test_sem_take_until_near_future(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);
	uint64_t before = ove_time_now_steady_ns();
	uint64_t deadline = before + OVE_MS(100);
	assert_int_equal(ove_sem_take_until(sem, deadline), OVE_ERR_TIMEOUT);
	uint64_t elapsed = ove_time_now_steady_ns() - before;
	/* One tick of slop for clock-read quantization. */
	assert_true(elapsed >= OVE_MS(90));
	ove_test_sem_destroy(sem);
}

/* ── _until: mutex lock with deadline — requires worker holding lock ── */

struct mtx_holder_ctx {
	ove_mutex_t mtx;
	volatile int holding;
	uint32_t hold_ms;
};

static void mtx_holder_entry(void *arg)
{
	struct mtx_holder_ctx *ctx = arg;
	OVE_TEST_LOCK(ctx->mtx);
	TEST_FLAG_SET(ctx->holding, 1);
	test_msleep(ctx->hold_ms);
	ove_mutex_unlock(ctx->mtx);
}

static void test_mutex_lock_until_near_future(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);

	/* Worker grabs the lock and holds it longer than the deadline. */
	struct mtx_holder_ctx ctx = {.mtx = mtx, .holding = 0, .hold_ms = 500};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "holder", mtx_holder_entry, &ctx, s_th_stack, 4096);
	assert_true(wait_for_flag(&ctx.holding, 1, 1000));

	uint64_t before = ove_time_now_steady_ns();
	uint64_t deadline = before + OVE_MS(100);
	assert_int_equal(ove_mutex_lock_until(mtx, deadline), OVE_ERR_TIMEOUT);
	uint64_t elapsed = ove_time_now_steady_ns() - before;
	assert_true(elapsed >= OVE_MS(90));

	ove_test_thread_destroy(th);
	ove_test_mutex_destroy(mtx);
}

/* ── _until: WAIT_FOREVER deadline blocks until signalled ───────────── */

struct sem_giver_ctx {
	ove_sem_t sem;
	uint32_t after_ms;
};

static void sem_giver_entry(void *arg)
{
	struct sem_giver_ctx *ctx = arg;
	test_msleep(ctx->after_ms);
	ove_sem_give(ctx->sem);
}

static void test_sem_take_until_wait_forever_unblocks(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);

	struct sem_giver_ctx ctx = {.sem = sem, .after_ms = 30};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage_b, "giver", sem_giver_entry, &ctx, s_th_stack_b,
			    4096);

	assert_int_equal(ove_sem_take_until(sem, OVE_WAIT_FOREVER), OVE_OK);

	ove_test_thread_destroy(th);
	ove_test_sem_destroy(sem);
}

/* ── _until: deadline arithmetic doesn't wrap near UINT64_MAX ───────── */

static void test_helper_deadline_near_uint64_max(void **state)
{
	(void)state;
	/* deadline = UINT64_MAX - 1: distinct from OVE_WAIT_FOREVER
	 * (=UINT64_MAX), so the helper should NOT treat as sentinel; it
	 * computes a huge remaining value without wrapping. */
	uint64_t deadline = UINT64_MAX - 1ULL;
	uint64_t remaining = ove_time_deadline_to_timeout_ns(deadline);
	assert_true(remaining != OVE_WAIT_FOREVER);
	/* Remaining must be approximately UINT64_MAX - now, which is
	 * astronomically large (centuries).  Check it's at least 1 year. */
	assert_true(remaining > OVE_SEC(31536000ULL));
}

/* ── runner ─────────────────────────────────────────────────────────── */

int test_deadline_until_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_helper_past_deadline_returns_zero),
		cmocka_unit_test(test_helper_wait_forever_passthrough),
		cmocka_unit_test(test_helper_future_deadline_returns_remaining),
		cmocka_unit_test(test_sem_take_until_past_deadline),
		cmocka_unit_test(test_event_wait_until_past_deadline),
		cmocka_unit_test(test_sem_take_until_near_future),
		cmocka_unit_test(test_mutex_lock_until_near_future),
		cmocka_unit_test(test_sem_take_until_wait_forever_unblocks),
		cmocka_unit_test(test_helper_deadline_near_uint64_max),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
