/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_mutex_storage_t, s_mtx_storage);
OVE_TEST_STORAGE(ove_mutex_storage_t, s_mtx_storage_a);
OVE_TEST_STORAGE(ove_mutex_storage_t, s_mtx_storage_b);
OVE_TEST_STORAGE(ove_mutex_storage_t, s_mtx_counter_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage_a);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage_b);
OVE_TEST_STACK(s_th_stack, 4096);
OVE_TEST_STACK(s_th_stack_a, 4096);
OVE_TEST_STACK(s_th_stack_b, 4096);

struct counter_ctx {
	ove_mutex_t mutex;
	int counter;
};

static void counter_entry(void *arg)
{
	struct counter_ctx *ctx = arg;
	for (int i = 0; i < 1000; i++) {
		OVE_TEST_LOCK(ctx->mutex);
		ctx->counter++;
		ove_mutex_unlock(ctx->mutex);
	}
}

struct hold_ctx {
	ove_mutex_t mutex;
	volatile int locked;
	volatile int released;
	int hold_ms;
};

static void hold_entry(void *arg)
{
	struct hold_ctx *ctx = arg;
	OVE_TEST_LOCK(ctx->mutex);
	TEST_FLAG_SET(ctx->locked, 1);
	test_msleep(ctx->hold_ms);
	ove_mutex_unlock(ctx->mutex);
	TEST_FLAG_SET(ctx->released, 1);
}

static void test_mutex_create(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	assert_int_equal(ove_test_mutex_create(&mtx, &s_mtx_storage), OVE_OK);
	assert_non_null(mtx);
	ove_test_mutex_destroy(mtx);
}

static void test_mutex_destroy_safe(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);
	ove_test_mutex_destroy(mtx);
}

static void test_mutex_lock_unlock(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);
	assert_int_equal(ove_mutex_lock(mtx, OVE_WAIT_FOREVER), OVE_OK);
	ove_mutex_unlock(mtx);
	ove_test_mutex_destroy(mtx);
}

static void test_mutex_lock_wait_forever(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);
	assert_int_equal(ove_mutex_lock(mtx, OVE_WAIT_FOREVER), OVE_OK);
	ove_mutex_unlock(mtx);
	ove_test_mutex_destroy(mtx);
}

static void test_mutex_contention_timeout(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);

	struct hold_ctx ctx = {.mutex = mtx, .hold_ms = 200};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "hold", hold_entry, &ctx, s_th_stack, 4096);
	assert_true(wait_for_flag(&ctx.locked, 1, 500));

	assert_int_equal(ove_mutex_lock(mtx, 50), OVE_ERR_TIMEOUT);

	ove_test_thread_destroy(th);
	ove_test_mutex_destroy(mtx);
}

static void test_mutex_contention_success(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);

	struct hold_ctx ctx = {.mutex = mtx, .hold_ms = 50};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "rel", hold_entry, &ctx, s_th_stack, 4096);
	assert_true(wait_for_flag(&ctx.locked, 1, 500));

	assert_int_equal(ove_mutex_lock(mtx, 500), OVE_OK);
	ove_mutex_unlock(mtx);

	ove_test_thread_destroy(th);
	ove_test_mutex_destroy(mtx);
}

static void test_mutex_double_unlock(void **state)
{
	(void)state;
	/* TSan correctly flags double-unlock as UB; this test is
	 * empirical "should not crash on double-unlock", which contradicts
	 * the formal rule.  Skip under TSan to keep the CI gate clean.
	 * Both gcc and clang define __SANITIZE_THREAD__ when built with
	 * -fsanitize=thread; that's the only macro we need. */
#ifdef __SANITIZE_THREAD__
	skip();
#else
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);
	OVE_TEST_LOCK(mtx);
	ove_mutex_unlock(mtx);
	ove_mutex_unlock(mtx); /* should not crash */
	ove_test_mutex_destroy(mtx);
#endif
}

static void test_mutex_zero_timeout_free(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);
	assert_int_equal(ove_mutex_lock(mtx, 0), OVE_OK);
	ove_mutex_unlock(mtx);
	ove_test_mutex_destroy(mtx);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_mutex_destroy_null(void **state)
{
	(void)state;
	ove_mutex_destroy(NULL);
}
#endif

static void test_mutex_shared_counter(void **state)
{
	(void)state;
	struct counter_ctx ctx = {0};
	ove_test_mutex_create(&ctx.mutex, &s_mtx_counter_storage);

	ove_thread_t t1 = NULL, t2 = NULL;
	ove_test_thread_run(&t1, &s_th_storage_a, "c1", counter_entry, &ctx, s_th_stack_a, 4096);
	ove_test_thread_run(&t2, &s_th_storage_b, "c2", counter_entry, &ctx, s_th_stack_b, 4096);
	ove_test_thread_destroy(t1);
	ove_test_thread_destroy(t2);

	assert_int_equal(ctx.counter, 2000);
	ove_test_mutex_destroy(ctx.mutex);
}

static void test_mutex_short_timeout(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_mutex_create(&mtx, &s_mtx_storage);

	struct hold_ctx ctx = {.mutex = mtx, .hold_ms = 200};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "h2", hold_entry, &ctx, s_th_stack, 4096);
	assert_true(wait_for_flag(&ctx.locked, 1, 500));

	uint64_t start = 0, end = 0;
	ove_time_get_us(&start);
	int rc = ove_mutex_lock(mtx, 50);
	ove_time_get_us(&end);
	uint64_t elapsed = end - start;

	assert_int_equal(rc, OVE_ERR_TIMEOUT);
	assert_duration_within(elapsed, 50, OVE_TEST_TIMING_TOLERANCE_MS);

	ove_test_thread_destroy(th);
	ove_test_mutex_destroy(mtx);
}

static void test_mutex_multiple_independent(void **state)
{
	(void)state;
	ove_mutex_t a = NULL, b = NULL;
	assert_int_equal(ove_test_mutex_create(&a, &s_mtx_storage_a), OVE_OK);
	assert_int_equal(ove_test_mutex_create(&b, &s_mtx_storage_b), OVE_OK);
	assert_int_equal(ove_mutex_lock(a, OVE_WAIT_FOREVER), OVE_OK);
	assert_int_equal(ove_mutex_lock(b, OVE_WAIT_FOREVER), OVE_OK);
	ove_mutex_unlock(b);
	ove_mutex_unlock(a);
	ove_test_mutex_destroy(a);
	ove_test_mutex_destroy(b);
}

int test_sync_mutex_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_mutex_create),
		cmocka_unit_test(test_mutex_destroy_safe),
		cmocka_unit_test(test_mutex_lock_unlock),
		cmocka_unit_test(test_mutex_lock_wait_forever),
		cmocka_unit_test(test_mutex_contention_timeout),
		cmocka_unit_test(test_mutex_contention_success),
		cmocka_unit_test(test_mutex_double_unlock),
		cmocka_unit_test(test_mutex_zero_timeout_free),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_mutex_destroy_null),
#endif
		cmocka_unit_test(test_mutex_shared_counter),
		cmocka_unit_test(test_mutex_short_timeout),
		cmocka_unit_test(test_mutex_multiple_independent),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
