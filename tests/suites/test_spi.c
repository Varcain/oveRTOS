/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

#ifdef CONFIG_OVE_SPI

/* ── tests ───────────────────────────────────────────────────────────── */

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_spi_create_destroy(void **state)
{
	(void)state;
	ove_spi_t spi;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	int rc = ove_spi_create(&spi, &cfg);
	assert_int_equal(rc, OVE_OK);
	ove_spi_destroy(spi);
}

static void test_spi_null_params(void **state)
{
	(void)state;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	int rc = ove_spi_create(NULL, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_spi_invalid_word_size(void **state)
{
	(void)state;
	ove_spi_t spi;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 32, /* invalid */
	};
	int rc = ove_spi_create(&spi, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_spi_write_read_wrappers(void **state)
{
	(void)state;
	ove_spi_t spi;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	ove_spi_create(&spi, &cfg);

	uint8_t data[] = {0x01, 0x02};
	int rc = ove_spi_write(spi, NULL, data, sizeof(data), 100);
	assert_int_equal(rc, OVE_OK);

	uint8_t buf[2];
	rc = ove_spi_read(spi, NULL, buf, sizeof(buf), 100);
	assert_int_equal(rc, OVE_OK);

	ove_spi_destroy(spi);
}
#else  /* CONFIG_OVE_ZERO_HEAP — use _init with caller-supplied storage */
static ove_spi_storage_t s_spi_cd;
static ove_spi_storage_t s_spi_iws;
static ove_spi_storage_t s_spi_wr;

static void test_spi_create_destroy(void **state)
{
	(void)state;
	ove_spi_t spi;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	int rc = ove_spi_init(&spi, &s_spi_cd, &cfg);
	assert_int_equal(rc, OVE_OK);
	ove_spi_deinit(spi);
}

static void test_spi_null_params(void **state)
{
	(void)state;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	int rc = ove_spi_init(NULL, &s_spi_cd, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_spi_invalid_word_size(void **state)
{
	(void)state;
	ove_spi_t spi;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 32, /* invalid */
	};
	int rc = ove_spi_init(&spi, &s_spi_iws, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_spi_write_read_wrappers(void **state)
{
	(void)state;
	ove_spi_t spi;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	ove_spi_init(&spi, &s_spi_wr, &cfg);

	uint8_t data[] = {0x01, 0x02};
	int rc = ove_spi_write(spi, NULL, data, sizeof(data), 100);
	assert_int_equal(rc, OVE_OK);

	uint8_t buf[2];
	rc = ove_spi_read(spi, NULL, buf, sizeof(buf), 100);
	assert_int_equal(rc, OVE_OK);

	ove_spi_deinit(spi);
}
#endif /* CONFIG_OVE_ZERO_HEAP */

static void test_spi_transfer_null_handle(void **state)
{
	(void)state;
	uint8_t tx[] = {0xAA, 0xBB};
	uint8_t rx[2];
	int rc = ove_spi_transfer(NULL, NULL, tx, rx, sizeof(tx), 100);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

#else /* !CONFIG_OVE_SPI */

static void test_spi_skipped(void **state)
{
	(void)state;
	skip();
}

#endif /* CONFIG_OVE_SPI */

/* ── runner ──────────────────────────────────────────────────────────── */

int test_spi_run(void)
{
#ifdef CONFIG_OVE_SPI
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_spi_create_destroy),
		cmocka_unit_test(test_spi_null_params),
		cmocka_unit_test(test_spi_invalid_word_size),
		cmocka_unit_test(test_spi_transfer_null_handle),
		cmocka_unit_test(test_spi_write_read_wrappers),
	};
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_spi_skipped),
	};
#endif
	return cmocka_run_group_tests(tests, NULL, NULL);
}
