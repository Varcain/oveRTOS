/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_mutex_storage_t, s_rmtx_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

struct rmtx_ctx {
	ove_mutex_t mtx;
	volatile int locked;
};

static void rmtx_hold_entry(void *arg)
{
	struct rmtx_ctx *ctx = arg;
	ove_recursive_mutex_lock(ctx->mtx, OVE_WAIT_FOREVER);
	ctx->locked = 1;
	test_msleep(200);
	ove_recursive_mutex_unlock(ctx->mtx);
}

static void test_recursive_create(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	assert_int_equal(ove_test_recursive_mutex_create(&mtx, &s_rmtx_storage), OVE_OK);
	ove_test_recursive_mutex_destroy(mtx);
}

static void test_recursive_lock_twice(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_recursive_mutex_create(&mtx, &s_rmtx_storage);
	assert_int_equal(ove_recursive_mutex_lock(mtx, OVE_WAIT_FOREVER), OVE_OK);
	assert_int_equal(ove_recursive_mutex_lock(mtx, OVE_WAIT_FOREVER), OVE_OK);
	ove_recursive_mutex_unlock(mtx);
	ove_recursive_mutex_unlock(mtx);
	ove_test_recursive_mutex_destroy(mtx);
}

static void test_recursive_matching_unlocks(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_recursive_mutex_create(&mtx, &s_rmtx_storage);
	for (int i = 0; i < 3; i++)
		ove_recursive_mutex_lock(mtx, OVE_WAIT_FOREVER);
	for (int i = 0; i < 3; i++)
		ove_recursive_mutex_unlock(mtx);
	assert_int_equal(ove_recursive_mutex_lock(mtx, 0), OVE_OK);
	ove_recursive_mutex_unlock(mtx);
	ove_test_recursive_mutex_destroy(mtx);
}

static void test_recursive_timeout(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_recursive_mutex_create(&mtx, &s_rmtx_storage);
	struct rmtx_ctx ctx = {.mtx = mtx};

	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "rh", rmtx_hold_entry, &ctx, s_th_stack, 4096);
	for (int i = 0; i < 2500 && !ctx.locked; i++)
		test_msleep(1);
	assert_int_equal(ctx.locked, 1);
	assert_int_equal(ove_recursive_mutex_lock(mtx, 50), OVE_ERR_TIMEOUT);
	ove_test_thread_destroy(th);
	ove_test_recursive_mutex_destroy(mtx);
}

static void test_recursive_destroy(void **state)
{
	(void)state;
	ove_mutex_t mtx = NULL;
	ove_test_recursive_mutex_create(&mtx, &s_rmtx_storage);
	ove_test_recursive_mutex_destroy(mtx);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_recursive_destroy_null(void **state)
{
	(void)state;
	ove_recursive_mutex_destroy(NULL);
}
#endif

int test_sync_recursive_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_recursive_create),
		cmocka_unit_test(test_recursive_lock_twice),
		cmocka_unit_test(test_recursive_matching_unlocks),
		cmocka_unit_test(test_recursive_timeout),
		cmocka_unit_test(test_recursive_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_recursive_destroy_null),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
