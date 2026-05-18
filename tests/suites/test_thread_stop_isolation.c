/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Verifies that ove_thread_request_stop / should_stop are per-thread:
 * requesting a stop on one thread must not affect a sibling.  Lives in
 * a separate file (STUB_ONLY category) from test_thread_stop.c because
 * it needs two simultaneous worker threads — two storage + stack
 * allocations push the renode-stm32f746-freertos RAM budget over the
 * edge.  POSIX coverage is sufficient: the isolation property is
 * established by the per-handle atomic flag in the substrate, not by
 * any backend specifically.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_thread_storage_t, s_th_a);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_b);
OVE_TEST_STACK(s_stack_a, 2048);
OVE_TEST_STACK(s_stack_b, 2048);

struct flag_ctx {
	ove_thread_t self;
	volatile int observed_false_before_request;
	volatile int observed_true_after_request;
	volatile int exited;
};

static void poll_worker(void *arg)
{
	struct flag_ctx *ctx = arg;
	while (__atomic_load_n(&ctx->self, __ATOMIC_ACQUIRE) == NULL)
		test_msleep(1);
	if (!ove_thread_should_stop(ctx->self))
		TEST_FLAG_SET(ctx->observed_false_before_request, 1);
	while (!ove_thread_should_stop(ctx->self))
		test_msleep(2);
	TEST_FLAG_SET(ctx->observed_true_after_request, 1);
	TEST_FLAG_SET(ctx->exited, 1);
}

static void test_request_stop_is_per_thread(void **state)
{
	(void)state;
	struct flag_ctx ctx_a = {NULL, 0, 0, 0};
	struct flag_ctx ctx_b = {NULL, 0, 0, 0};

	ove_thread_t th_a = NULL;
	ove_thread_t th_b = NULL;
	ove_test_thread_run(&th_a, &s_th_a, "poll_a", poll_worker, &ctx_a, s_stack_a, 2048);
	ove_test_thread_run(&th_b, &s_th_b, "poll_b", poll_worker, &ctx_b, s_stack_b, 2048);
	__atomic_store_n(&ctx_a.self, th_a, __ATOMIC_RELEASE);
	__atomic_store_n(&ctx_b.self, th_b, __ATOMIC_RELEASE);
	assert_true(wait_for_flag(&ctx_a.observed_false_before_request, 1, 1000));
	assert_true(wait_for_flag(&ctx_b.observed_false_before_request, 1, 1000));

	/* Request stop on A only. */
	ove_thread_request_stop(th_a);

	/* A exits; B keeps running. */
	assert_true(wait_for_flag(&ctx_a.exited, 1, 1000));
	test_msleep(50); /* give B time to also observe a stop — it shouldn't */
	assert_int_equal(ctx_b.exited, 0);
	assert_false(ove_thread_should_stop(th_b));

	/* Now stop B too. */
	ove_thread_request_stop(th_b);
	assert_true(wait_for_flag(&ctx_b.exited, 1, 1000));

	ove_test_thread_destroy(th_a);
	ove_test_thread_destroy(th_b);
}

int test_thread_stop_isolation_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_request_stop_is_per_thread),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
