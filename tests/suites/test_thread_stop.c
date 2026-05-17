/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Coverage for cooperative cancellation (Phase 4):
 *   - ove_thread_request_stop(handle) sets a per-thread atomic flag.
 *   - ove_thread_should_stop(handle) reads it.
 *   - The substrate does NOT force-terminate; workers must poll
 *     should_stop in their loop and exit voluntarily.
 *   - Flag is per-thread (request on one doesn't affect siblings).
 *   - Safe from any context (worker self, parent thread, ISR-like).
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_thread_storage_t, s_th_a);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_b);
OVE_TEST_STACK(s_stack_a, 4096);
OVE_TEST_STACK(s_stack_b, 4096);

/* ── default state: should_stop is false ────────────────────────────── */

struct flag_ctx {
	volatile int observed_false_before_request;
	volatile int observed_true_after_request;
	volatile int exited;
};

static void poll_worker(void *arg)
{
	struct flag_ctx *ctx = arg;
	ove_thread_t self = ove_thread_get_self();
	/* On entry: must observe false. */
	if (!ove_thread_should_stop(self))
		TEST_FLAG_SET(ctx->observed_false_before_request, 1);
	/* Poll loop. */
	while (!ove_thread_should_stop(self))
		test_msleep(2);
	TEST_FLAG_SET(ctx->observed_true_after_request, 1);
	TEST_FLAG_SET(ctx->exited, 1);
}

static void test_should_stop_false_on_entry_then_true_after_request(void **state)
{
	(void)state;
	struct flag_ctx ctx = {0, 0, 0};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_a, "poller", poll_worker, &ctx, s_stack_a, 4096);

	/* Worker observes false on entry within a few ms. */
	assert_true(wait_for_flag(&ctx.observed_false_before_request, 1, 1000));

	/* Request stop; worker should observe true and exit. */
	ove_thread_request_stop(th);
	assert_true(wait_for_flag(&ctx.observed_true_after_request, 1, 1000));
	assert_true(wait_for_flag(&ctx.exited, 1, 1000));

	/* request_stop is sticky — querying after the worker exits still
	 * returns true (kernel state intact until destroy). */
	assert_true(ove_thread_should_stop(th));

	ove_test_thread_destroy(th);
}

/* ── request_stop on one thread doesn't affect another ──────────────── */

static void test_request_stop_is_per_thread(void **state)
{
	(void)state;
	struct flag_ctx ctx_a = {0, 0, 0};
	struct flag_ctx ctx_b = {0, 0, 0};

	ove_thread_t th_a = NULL;
	ove_thread_t th_b = NULL;
	ove_test_thread_run(&th_a, &s_th_a, "poll_a", poll_worker, &ctx_a, s_stack_a, 4096);
	ove_test_thread_run(&th_b, &s_th_b, "poll_b", poll_worker, &ctx_b, s_stack_b, 4096);
	assert_true(wait_for_flag(&ctx_a.observed_false_before_request, 1, 1000));
	assert_true(wait_for_flag(&ctx_b.observed_false_before_request, 1, 1000));

	/* Request stop on A only. */
	ove_thread_request_stop(th_a);

	/* A exits; B keeps running. */
	assert_true(wait_for_flag(&ctx_a.exited, 1, 1000));
	test_msleep(50);  /* give B time to also observe a stop — it shouldn't */
	assert_int_equal(ctx_b.exited, 0);
	assert_false(ove_thread_should_stop(th_b));

	/* Now stop B too. */
	ove_thread_request_stop(th_b);
	assert_true(wait_for_flag(&ctx_b.exited, 1, 1000));

	ove_test_thread_destroy(th_a);
	ove_test_thread_destroy(th_b);
}

/* ── self-request: worker can call on itself ────────────────────────── */

static void self_stop_worker(void *arg)
{
	struct flag_ctx *ctx = arg;
	ove_thread_t self = ove_thread_get_self();
	test_msleep(20);
	ove_thread_request_stop(self);
	assert_true(ove_thread_should_stop(self));
	TEST_FLAG_SET(ctx->exited, 1);
}

static void test_thread_can_request_stop_on_self(void **state)
{
	(void)state;
	struct flag_ctx ctx = {0, 0, 0};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_a, "self_stop", self_stop_worker, &ctx, s_stack_a, 4096);
	assert_true(wait_for_flag(&ctx.exited, 1, 1000));
	assert_true(ove_thread_should_stop(th));
	ove_test_thread_destroy(th);
}

/* ── NULL handle: must not crash ────────────────────────────────────── */

static void test_request_stop_null_handle_is_noop(void **state)
{
	(void)state;
	ove_thread_request_stop(NULL);
	assert_false(ove_thread_should_stop(NULL));
}

/* ── runner ─────────────────────────────────────────────────────────── */

int test_thread_stop_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_should_stop_false_on_entry_then_true_after_request),
		cmocka_unit_test(test_request_stop_is_per_thread),
		cmocka_unit_test(test_thread_can_request_stop_on_self),
		cmocka_unit_test(test_request_stop_null_handle_is_noop),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
