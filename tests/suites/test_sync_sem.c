/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_sem_storage_t, s_sem_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

struct sem_ctx {
	ove_sem_t sem;
	volatile int done;
};

static void sem_give_entry(void *arg)
{
	struct sem_ctx *ctx = arg;
	test_msleep(50);
	ove_sem_give(ctx->sem);
	ctx->done = 1;
}

static void sem_give_delayed_entry(void *arg)
{
	struct sem_ctx *ctx = arg;
	test_msleep(100);
	ove_sem_give(ctx->sem);
	ctx->done = 1;
}

static void test_sem_create_binary(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	assert_int_equal(ove_test_sem_create(&sem, &s_sem_storage, 1, 1), OVE_OK);
	ove_test_sem_destroy(sem);
}

static void test_sem_create_counting(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	assert_int_equal(ove_test_sem_create(&sem, &s_sem_storage, 0, 10), OVE_OK);
	ove_test_sem_destroy(sem);
}

static void test_sem_take_initial_one(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 1, 1);
	assert_int_equal(ove_sem_take(sem, 0), OVE_OK);
	ove_test_sem_destroy(sem);
}

static void test_sem_take_timeout(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 10);
	assert_int_equal(ove_sem_take(sem, OVE_MS(50)), OVE_ERR_TIMEOUT);
	ove_test_sem_destroy(sem);
}

static void test_sem_give_then_take(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 10);
	ove_sem_give(sem);
	assert_int_equal(ove_sem_take(sem, 0), OVE_OK);
	ove_test_sem_destroy(sem);
}

static void test_sem_counting(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 10);
	for (int i = 0; i < 3; i++)
		ove_sem_give(sem);
	for (int i = 0; i < 3; i++)
		assert_int_equal(ove_sem_take(sem, 0), OVE_OK);
	assert_int_equal(ove_sem_take(sem, OVE_MS(10)), OVE_ERR_TIMEOUT);
	ove_test_sem_destroy(sem);
}

static void test_sem_producer_consumer(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);
	struct sem_ctx ctx = {.sem = sem};

	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "prod", sem_give_entry, &ctx, s_th_stack, 4096);
	assert_int_equal(ove_sem_take(sem, OVE_MS(500)), OVE_OK);
	ove_test_thread_destroy(th);
	assert_int_equal(ctx.done, 1);
	ove_test_sem_destroy(sem);
}

static void test_sem_destroy_basic(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 1, 1);
	ove_test_sem_destroy(sem);
}

static void test_sem_wait_forever(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);
	struct sem_ctx ctx = {.sem = sem};

	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "wf", sem_give_delayed_entry, &ctx, s_th_stack,
			    4096);
	assert_int_equal(ove_sem_take(sem, OVE_WAIT_FOREVER), OVE_OK);
	ove_test_thread_destroy(th);
	ove_test_sem_destroy(sem);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_sem_destroy_null(void **state)
{
	(void)state;
	ove_sem_destroy(NULL);
}

#endif

int test_sync_sem_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_sem_create_binary),
		cmocka_unit_test(test_sem_create_counting),
		cmocka_unit_test(test_sem_take_initial_one),
		cmocka_unit_test(test_sem_take_timeout),
		cmocka_unit_test(test_sem_give_then_take),
		cmocka_unit_test(test_sem_counting),
		cmocka_unit_test(test_sem_producer_consumer),
		cmocka_unit_test(test_sem_destroy_basic),
		cmocka_unit_test(test_sem_wait_forever),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_sem_destroy_null),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
