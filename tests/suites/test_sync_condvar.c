/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_condvar_storage_t, s_cv_storage);
OVE_TEST_STORAGE(ove_mutex_storage_t, s_mtx_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage_a);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage_b);
OVE_TEST_STACK(s_th_stack_a, 4096);
OVE_TEST_STACK(s_th_stack_b, 4096);

struct cv_waiter_ctx {
	ove_condvar_t cv;
	ove_mutex_t mtx;
	volatile int woke;
	volatile int ready; /* set by waiter before entering condvar_wait */
};

static void cv_wait_entry(void *arg)
{
	struct cv_waiter_ctx *ctx = arg;
	OVE_TEST_LOCK(ctx->mtx);
	ctx->ready = 1;
	OVE_TEST_CONDVAR_WAIT(ctx->cv, ctx->mtx);
	ctx->woke = 1;
	ove_mutex_unlock(ctx->mtx);
}

struct cv_signal_ctx {
	ove_condvar_t cv;
	ove_mutex_t mtx;
	volatile int signaled;
};

static void cv_signal_entry(void *arg)
{
	struct cv_signal_ctx *ctx = arg;
	test_msleep(50);
	OVE_TEST_LOCK(ctx->mtx);
	ctx->signaled = 1;
	ove_condvar_signal(ctx->cv);
	ove_mutex_unlock(ctx->mtx);
}

struct cv_prod_ctx {
	ove_condvar_t cv;
	ove_mutex_t mtx;
	volatile int ready;
};

static void cv_producer_entry(void *arg)
{
	struct cv_prod_ctx *ctx = arg;
	test_msleep(50);
	OVE_TEST_LOCK(ctx->mtx);
	ctx->ready = 1;
	ove_condvar_signal(ctx->cv);
	ove_mutex_unlock(ctx->mtx);
}

static void test_condvar_create(void **state)
{
	(void)state;
	ove_condvar_t cv = NULL;
	assert_int_equal(ove_test_condvar_create(&cv, &s_cv_storage), OVE_OK);
	ove_test_condvar_destroy(cv);
}

static void test_condvar_destroy_basic(void **state)
{
	(void)state;
	ove_condvar_t cv = NULL;
	ove_test_condvar_create(&cv, &s_cv_storage);
	ove_test_condvar_destroy(cv);
}

static void test_condvar_signal_wakes_one(void **state)
{
	(void)state;
	ove_condvar_t cv = NULL;
	ove_mutex_t mtx = NULL;
	ove_test_condvar_create(&cv, &s_cv_storage);
	ove_test_mutex_create(&mtx, &s_mtx_storage);

	struct cv_waiter_ctx ctx = {.cv = cv, .mtx = mtx};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "cvw", cv_wait_entry, &ctx, s_th_stack, 4096);

	/* Wait until the waiter has entered condvar_wait (holds mutex, sets ready) */
	for (int i = 0; i < 500; i++) {
		OVE_TEST_LOCK(mtx);
		int rdy = ctx.ready;
		ove_mutex_unlock(mtx);
		if (rdy)
			break;
		test_msleep(5);
	}
	assert_int_equal(ctx.ready, 1);

	OVE_TEST_LOCK(mtx);
	ove_condvar_signal(cv);
	ove_mutex_unlock(mtx);

	ove_test_thread_destroy(th);
	assert_int_equal(ctx.woke, 1);
	ove_test_condvar_destroy(cv);
	ove_test_mutex_destroy(mtx);
}

static void test_condvar_broadcast(void **state)
{
	(void)state;
	ove_condvar_t cv = NULL;
	ove_mutex_t mtx = NULL;
	ove_test_condvar_create(&cv, &s_cv_storage);
	ove_test_mutex_create(&mtx, &s_mtx_storage);

	struct cv_waiter_ctx c1 = {.cv = cv, .mtx = mtx};
	struct cv_waiter_ctx c2 = {.cv = cv, .mtx = mtx};

	ove_thread_t t1 = NULL, t2 = NULL;
	ove_test_thread_run(&t1, &s_th_storage_a, "w1", cv_wait_entry, &c1, s_th_stack_a, 4096);
	ove_test_thread_run(&t2, &s_th_storage_b, "w2", cv_wait_entry, &c2, s_th_stack_b, 4096);

	/* Wait until both waiters are ready (inside condvar_wait) */
	for (int i = 0; i < 500; i++) {
		OVE_TEST_LOCK(mtx);
		int both_ready = c1.ready && c2.ready;
		ove_mutex_unlock(mtx);
		if (both_ready)
			break;
		test_msleep(5);
	}
	assert_int_equal(c1.ready, 1);
	assert_int_equal(c2.ready, 1);

	OVE_TEST_LOCK(mtx);
	ove_condvar_broadcast(cv);
	ove_mutex_unlock(mtx);

	ove_test_thread_destroy(t1);
	ove_test_thread_destroy(t2);
	assert_int_equal(c1.woke, 1);
	assert_int_equal(c2.woke, 1);
	ove_test_condvar_destroy(cv);
	ove_test_mutex_destroy(mtx);
}

static void test_condvar_wait_timeout(void **state)
{
	(void)state;
	ove_condvar_t cv = NULL;
	ove_mutex_t mtx = NULL;
	ove_test_condvar_create(&cv, &s_cv_storage);
	ove_test_mutex_create(&mtx, &s_mtx_storage);
	OVE_TEST_LOCK(mtx);
	assert_int_equal(ove_condvar_wait(cv, mtx, OVE_MS(50)), OVE_ERR_TIMEOUT);
	ove_mutex_unlock(mtx);
	ove_test_condvar_destroy(cv);
	ove_test_mutex_destroy(mtx);
}

static void test_condvar_producer_consumer(void **state)
{
	(void)state;
	ove_condvar_t cv = NULL;
	ove_mutex_t mtx = NULL;
	ove_test_condvar_create(&cv, &s_cv_storage);
	ove_test_mutex_create(&mtx, &s_mtx_storage);

	struct cv_prod_ctx ctx = {.cv = cv, .mtx = mtx};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "prod", cv_producer_entry, &ctx, s_th_stack, 4096);

	OVE_TEST_LOCK(mtx);
	while (!ctx.ready)
		OVE_TEST_CONDVAR_WAIT(cv, mtx);
	ove_mutex_unlock(mtx);

	ove_test_thread_destroy(th);
	assert_int_equal(ctx.ready, 1);
	ove_test_condvar_destroy(cv);
	ove_test_mutex_destroy(mtx);
}

static void test_condvar_wait_forever(void **state)
{
	(void)state;
	ove_condvar_t cv = NULL;
	ove_mutex_t mtx = NULL;
	ove_test_condvar_create(&cv, &s_cv_storage);
	ove_test_mutex_create(&mtx, &s_mtx_storage);

	struct cv_signal_ctx ctx = {.cv = cv, .mtx = mtx};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "sig", cv_signal_entry, &ctx, s_th_stack, 4096);

	OVE_TEST_LOCK(mtx);
	assert_int_equal(ove_condvar_wait(cv, mtx, OVE_WAIT_FOREVER), OVE_OK);
	ove_mutex_unlock(mtx);

	ove_test_thread_destroy(th);
	assert_int_equal(ctx.signaled, 1);
	ove_test_condvar_destroy(cv);
	ove_test_mutex_destroy(mtx);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_condvar_destroy_null(void **state)
{
	(void)state;
	ove_condvar_destroy(NULL);
}

#endif

int test_sync_condvar_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_condvar_create),
		cmocka_unit_test(test_condvar_destroy_basic),
		cmocka_unit_test(test_condvar_signal_wakes_one),
		cmocka_unit_test(test_condvar_broadcast),
		cmocka_unit_test(test_condvar_wait_timeout),
		cmocka_unit_test(test_condvar_producer_consumer),
		cmocka_unit_test(test_condvar_wait_forever),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_condvar_destroy_null),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
