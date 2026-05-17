/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_event_storage_t, s_evt_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

struct evt_ctx {
	ove_event_t evt;
	volatile int done;
};

static void evt_signal_entry(void *arg)
{
	struct evt_ctx *ctx = arg;
	test_msleep(50);
	ove_event_signal(ctx->evt);
	ctx->done = 1;
}

static void test_event_create(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	assert_int_equal(ove_test_event_create(&evt, &s_evt_storage), OVE_OK);
	ove_test_event_destroy(evt);
}

static void test_event_destroy_basic(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	ove_test_event_create(&evt, &s_evt_storage);
	ove_test_event_destroy(evt);
}

static void test_event_signal_then_wait(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	ove_test_event_create(&evt, &s_evt_storage);
	ove_event_signal(evt);
	assert_int_equal(ove_event_wait(evt, 0), OVE_OK);
	ove_test_event_destroy(evt);
}

static void test_event_wait_timeout(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	ove_test_event_create(&evt, &s_evt_storage);
	assert_int_equal(ove_event_wait(evt, 50), OVE_ERR_TIMEOUT);
	ove_test_event_destroy(evt);
}

static void test_event_cross_thread(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	ove_test_event_create(&evt, &s_evt_storage);
	struct evt_ctx ctx = {.evt = evt};

	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "esig", evt_signal_entry, &ctx, s_th_stack, 4096);
	assert_int_equal(ove_event_wait(evt, 500), OVE_OK);
	ove_test_thread_destroy(th);
	assert_int_equal(ctx.done, 1);
	ove_test_event_destroy(evt);
}

static void test_event_signal_from_isr(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	ove_test_event_create(&evt, &s_evt_storage);
	ove_event_signal_from_isr(evt);
	assert_int_equal(ove_event_wait(evt, 0), OVE_OK);
	ove_test_event_destroy(evt);
}

static void test_event_auto_reset(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	ove_test_event_create(&evt, &s_evt_storage);
	ove_event_signal(evt);
	assert_int_equal(ove_event_wait(evt, 0), OVE_OK);
	assert_int_equal(ove_event_wait(evt, 50), OVE_ERR_TIMEOUT);
	ove_test_event_destroy(evt);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_event_destroy_null(void **state)
{
	(void)state;
	ove_event_destroy(NULL);
}

#endif

int test_sync_event_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_event_create),
		cmocka_unit_test(test_event_destroy_basic),
		cmocka_unit_test(test_event_signal_then_wait),
		cmocka_unit_test(test_event_wait_timeout),
		cmocka_unit_test(test_event_cross_thread),
		cmocka_unit_test(test_event_signal_from_isr),
		cmocka_unit_test(test_event_auto_reset),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_event_destroy_null),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
