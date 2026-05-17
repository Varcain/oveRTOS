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
 *   - Safe from any context (worker self, parent thread, ISR-like).
 *
 * NB: the per-thread isolation case (two simultaneous worker threads,
 * stopping one does not affect the other) lives in
 * test_thread_stop_isolation.c — that test needs two thread storage +
 * stack allocations and pushes the renode-stm32f746 RAM budget over
 * the edge.  Single-backend POSIX coverage in the STUB_ONLY list is
 * sufficient for the isolation property (it's a property of the
 * per-handle atomic flag, not backend-specific).
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_thread_storage_t, s_th_a);
OVE_TEST_STACK(s_stack_a, 2048);

/* ── default state: should_stop is false ────────────────────────────── */

struct flag_ctx {
	/* Handle of the worker thread itself.  Filled by the parent before
	 * spawn; the worker uses this instead of ove_thread_get_self() to
	 * avoid a race on backends (e.g. FreeRTOS) where the per-thread
	 * "self" lookup is populated by the parent AFTER xTaskCreate
	 * returns — the worker can start running and call get_self() before
	 * that completes, yielding NULL. */
	ove_thread_t self;
	volatile int observed_false_before_request;
	volatile int observed_true_after_request;
	volatile int exited;
};

static void poll_worker(void *arg)
{
	struct flag_ctx *ctx = arg;
	/* Wait until the parent fills ctx->self.  In practice the parent
	 * stores it immediately after the spawn helper returns, so by the
	 * time the worker is dispatched this is usually already set. */
	while (__atomic_load_n(&ctx->self, __ATOMIC_ACQUIRE) == NULL)
		test_msleep(1);
	/* On entry: must observe false. */
	if (!ove_thread_should_stop(ctx->self))
		TEST_FLAG_SET(ctx->observed_false_before_request, 1);
	/* Poll loop. */
	while (!ove_thread_should_stop(ctx->self))
		test_msleep(2);
	TEST_FLAG_SET(ctx->observed_true_after_request, 1);
	TEST_FLAG_SET(ctx->exited, 1);
}

static void test_should_stop_false_on_entry_then_true_after_request(void **state)
{
	(void)state;
	struct flag_ctx ctx = {NULL, 0, 0, 0};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_a, "poller", poll_worker, &ctx, s_stack_a, 2048);
	__atomic_store_n(&ctx.self, th, __ATOMIC_RELEASE);

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

/* ── self-request: worker can call on itself ────────────────────────── */

static void self_stop_worker(void *arg)
{
	struct flag_ctx *ctx = arg;
	/* Wait for parent to fill ctx->self (see note on poll_worker). */
	ove_thread_t self;
	while ((self = __atomic_load_n(&ctx->self, __ATOMIC_ACQUIRE)) == NULL)
		test_msleep(1);
	test_msleep(20);
	ove_thread_request_stop(self);
	assert_true(ove_thread_should_stop(self));
	TEST_FLAG_SET(ctx->exited, 1);
}

static void test_thread_can_request_stop_on_self(void **state)
{
	(void)state;
	struct flag_ctx ctx = {NULL, 0, 0, 0};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_a, "self_stop", self_stop_worker, &ctx, s_stack_a, 2048);
	__atomic_store_n(&ctx.self, th, __ATOMIC_RELEASE);
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
		cmocka_unit_test(test_thread_can_request_stop_on_self),
		cmocka_unit_test(test_request_stop_null_handle_is_noop),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
