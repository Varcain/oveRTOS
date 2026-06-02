/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_eventgroup_storage_t, s_eg_storage);
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

/* ── helpers ─────────────────────────────────────────────────────────── */

#define BIT_0 (1u << 0)
#define BIT_1 (1u << 1)
#define BIT_2 (1u << 2)

typedef struct {
	ove_eventgroup_t eg;
	uint32_t bits_to_set;
	uint32_t delay_ms;
} setter_ctx_t;

static void setter_thread(void *arg)
{
	setter_ctx_t *ctx = (setter_ctx_t *)arg;
	test_msleep(ctx->delay_ms);
	ove_eventgroup_set_bits(ctx->eg, ctx->bits_to_set);
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_eg_create_destroy(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	int rc = ove_test_eventgroup_create(&eg, &s_eg_storage);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(eg);
	ove_test_eventgroup_destroy(eg);
}

static void test_eg_set_bits_returns_updated(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	ove_eventgroup_set_bits(eg, BIT_0 | BIT_1);
	ove_eventbits_t bits = ove_eventgroup_get_bits(eg);
	assert_true(bits & BIT_0);
	assert_true(bits & BIT_1);

	ove_test_eventgroup_destroy(eg);
}

static void test_eg_clear_bits_returns_previous(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	ove_eventgroup_set_bits(eg, BIT_0 | BIT_1 | BIT_2);

	/* clear_bits returns the value as it was *before* the clear (contract
	 * in ove/eventgroup.h; matches FreeRTOS xEventGroupClearBits). */
	ove_eventbits_t prev = ove_eventgroup_clear_bits(eg, BIT_1);
	assert_int_equal(prev, BIT_0 | BIT_1 | BIT_2);

	ove_eventbits_t remaining = ove_eventgroup_get_bits(eg);
	/* BIT_1 should now be cleared */
	assert_true(remaining & BIT_0);
	assert_false(remaining & BIT_1);
	assert_true(remaining & BIT_2);

	ove_test_eventgroup_destroy(eg);
}

static void test_eg_get_bits(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	ove_eventbits_t bits = ove_eventgroup_get_bits(eg);
	assert_int_equal(bits, 0);

	ove_eventgroup_set_bits(eg, BIT_2);
	bits = ove_eventgroup_get_bits(eg);
	assert_true(bits & BIT_2);

	ove_test_eventgroup_destroy(eg);
}

static void test_eg_wait_all(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	ove_eventgroup_set_bits(eg, BIT_0 | BIT_1);

	ove_eventbits_t actual = 0;
	int rc = ove_eventgroup_wait_bits(eg, BIT_0 | BIT_1, OVE_EG_WAIT_ALL, OVE_MS(100), &actual);
	assert_int_equal(rc, OVE_OK);
	assert_true((actual & (BIT_0 | BIT_1)) == (BIT_0 | BIT_1));

	ove_test_eventgroup_destroy(eg);
}

static void test_eg_wait_any(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	ove_eventgroup_set_bits(eg, BIT_0);

	ove_eventbits_t actual = 0;
	int rc = ove_eventgroup_wait_bits(eg, BIT_0 | BIT_1, 0, OVE_MS(100), &actual);
	assert_int_equal(rc, OVE_OK);
	assert_true(actual & BIT_0);

	ove_test_eventgroup_destroy(eg);
}

static void test_eg_wait_timeout(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	ove_eventbits_t actual = 0;
	int rc = ove_eventgroup_wait_bits(eg, BIT_0, OVE_EG_WAIT_ALL, OVE_MS(10), &actual);
	assert_int_equal(rc, OVE_ERR_TIMEOUT);

	ove_test_eventgroup_destroy(eg);
}

static void test_eg_clear_on_exit(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	ove_eventgroup_set_bits(eg, BIT_0 | BIT_1);

	ove_eventbits_t actual = 0;
	int rc = ove_eventgroup_wait_bits(eg, BIT_0 | BIT_1, OVE_EG_WAIT_ALL | OVE_EG_CLEAR_ON_EXIT,
					  OVE_MS(100), &actual);
	assert_int_equal(rc, OVE_OK);

	/* After CLEAR_ON_EXIT the waited bits should be cleared */
	ove_eventbits_t remaining = ove_eventgroup_get_bits(eg);
	assert_false(remaining & BIT_0);
	assert_false(remaining & BIT_1);

	ove_test_eventgroup_destroy(eg);
}

static void test_eg_set_bits_from_isr(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	ove_eventgroup_set_bits_from_isr(eg, BIT_2);

	ove_eventbits_t bits = ove_eventgroup_get_bits(eg);
	assert_true(bits & BIT_2);

	ove_test_eventgroup_destroy(eg);
}

static void test_eg_cross_thread(void **state)
{
	(void)state;
	ove_eventgroup_t eg = NULL;
	ove_test_eventgroup_create(&eg, &s_eg_storage);

	setter_ctx_t ctx = {
		.eg = eg,
		.bits_to_set = BIT_0 | BIT_1,
		.delay_ms = 50,
	};

	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "setter", setter_thread, &ctx, s_th_stack, 4096);

	ove_eventbits_t actual = 0;
	int rc = ove_eventgroup_wait_bits(eg, BIT_0 | BIT_1, OVE_EG_WAIT_ALL, OVE_MS(500), &actual);
	assert_int_equal(rc, OVE_OK);
	assert_true((actual & (BIT_0 | BIT_1)) == (BIT_0 | BIT_1));

	ove_test_thread_destroy(th);
	ove_test_eventgroup_destroy(eg);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_eg_create_null_handle(void **state)
{
	(void)state;
	int rc = ove_eventgroup_create(NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}
#endif

/* ── runner ──────────────────────────────────────────────────────────── */

int test_eventgroup_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_eg_create_destroy),
		cmocka_unit_test(test_eg_set_bits_returns_updated),
		cmocka_unit_test(test_eg_clear_bits_returns_previous),
		cmocka_unit_test(test_eg_get_bits),
		cmocka_unit_test(test_eg_wait_all),
		cmocka_unit_test(test_eg_wait_any),
		cmocka_unit_test(test_eg_wait_timeout),
		cmocka_unit_test(test_eg_clear_on_exit),
		cmocka_unit_test(test_eg_set_bits_from_isr),
		cmocka_unit_test(test_eg_cross_thread),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_eg_create_null_handle),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
