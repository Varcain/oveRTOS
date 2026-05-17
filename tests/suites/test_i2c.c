/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

#ifdef CONFIG_OVE_I2C

/* ── tests ───────────────────────────────────────────────────────────── */

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_i2c_create_destroy(void **state)
{
	(void)state;
	ove_i2c_t i2c;
	struct ove_i2c_cfg cfg = {
		.instance = 0,
		.speed = OVE_I2C_SPEED_STANDARD,
	};
	int rc = ove_i2c_create(&i2c, &cfg);
	assert_int_equal(rc, OVE_OK);
	ove_i2c_destroy(i2c);
}

static void test_i2c_null_params(void **state)
{
	(void)state;
	struct ove_i2c_cfg cfg = {.instance = 0, .speed = OVE_I2C_SPEED_FAST};
	int rc = ove_i2c_create(NULL, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	rc = ove_i2c_create(NULL, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_i2c_read_null_buf(void **state)
{
	(void)state;
	ove_i2c_t i2c;
	struct ove_i2c_cfg cfg = {.instance = 0, .speed = OVE_I2C_SPEED_STANDARD};
	ove_i2c_create(&i2c, &cfg);

	int rc = ove_i2c_read(i2c, 0x50, NULL, 4, OVE_MS(100));
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_i2c_destroy(i2c);
}
#else  /* CONFIG_OVE_ZERO_HEAP — use _init with caller-supplied storage */
static ove_i2c_storage_t s_i2c_cd;
static ove_i2c_storage_t s_i2c_rd;

static void test_i2c_create_destroy(void **state)
{
	(void)state;
	ove_i2c_t i2c;
	struct ove_i2c_cfg cfg = {
		.instance = 0,
		.speed = OVE_I2C_SPEED_STANDARD,
	};
	int rc = ove_i2c_init(&i2c, &s_i2c_cd, &cfg);
	assert_int_equal(rc, OVE_OK);
	ove_i2c_deinit(i2c);
}

static void test_i2c_null_params(void **state)
{
	(void)state;
	struct ove_i2c_cfg cfg = {.instance = 0, .speed = OVE_I2C_SPEED_FAST};
	int rc = ove_i2c_init(NULL, &s_i2c_cd, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	rc = ove_i2c_init(NULL, NULL, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_i2c_read_null_buf(void **state)
{
	(void)state;
	ove_i2c_t i2c;
	struct ove_i2c_cfg cfg = {.instance = 0, .speed = OVE_I2C_SPEED_STANDARD};
	ove_i2c_init(&i2c, &s_i2c_rd, &cfg);

	int rc = ove_i2c_read(i2c, 0x50, NULL, 4, OVE_MS(100));
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_i2c_deinit(i2c);
}
#endif /* CONFIG_OVE_ZERO_HEAP */

static void test_i2c_write_null_handle(void **state)
{
	(void)state;
	uint8_t data[] = {0x00, 0x01};
	int rc = ove_i2c_write(NULL, 0x50, data, sizeof(data), OVE_MS(100));
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_i2c_probe_null_handle(void **state)
{
	(void)state;
	int rc = ove_i2c_probe(NULL, 0x50, OVE_MS(100));
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

#else /* !CONFIG_OVE_I2C */

static void test_i2c_skipped(void **state)
{
	(void)state;
	skip();
}

#endif /* CONFIG_OVE_I2C */

/* ── runner ──────────────────────────────────────────────────────────── */

int test_i2c_run(void)
{
#ifdef CONFIG_OVE_I2C
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_i2c_create_destroy),
		cmocka_unit_test(test_i2c_null_params),
		cmocka_unit_test(test_i2c_write_null_handle),
		cmocka_unit_test(test_i2c_read_null_buf),
		cmocka_unit_test(test_i2c_probe_null_handle),
	};
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_i2c_skipped),
	};
#endif
	return cmocka_run_group_tests(tests, NULL, NULL);
}
