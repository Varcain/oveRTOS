/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

#ifdef CONFIG_OVE_UART

/* ── tests ───────────────────────────────────────────────────────────── */

/*
 * In zero-heap mode `ove_uart_create(p, cfg)` expands to a per-call-site
 * `static uint8_t buf[(cfg)->rx_buf_size]` — a VLA in standard C — so cfg
 * must have a literal compile-time `rx_buf_size`.  Test cases that pass
 * a runtime cfg struct can't satisfy that and are gated to heap mode;
 * the equivalent zero-heap variants below use `ove_uart_init` directly
 * with caller-supplied static storage.
 */

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_uart_create_destroy(void **state)
{
	(void)state;
	ove_uart_t uart;
	struct ove_uart_cfg cfg = {
		.instance = 0,
		.baudrate = 115200,
		.data_bits = 8,
		.parity = OVE_UART_PARITY_NONE,
		.stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE,
		.rx_buf_size = 64,
	};
	int rc = ove_uart_create(&uart, &cfg);
	assert_int_equal(rc, OVE_OK);
	ove_uart_destroy(uart);
}

static void test_uart_null_params(void **state)
{
	(void)state;
	struct ove_uart_cfg cfg = {
		.instance = 0, .baudrate = 115200, .data_bits = 8,
		.parity = OVE_UART_PARITY_NONE, .stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE, .rx_buf_size = 64,
	};
	int rc = ove_uart_create(NULL, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_uart_zero_baudrate(void **state)
{
	(void)state;
	ove_uart_t uart;
	struct ove_uart_cfg cfg = {
		.instance = 0, .baudrate = 0, .data_bits = 8,
		.parity = OVE_UART_PARITY_NONE, .stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE, .rx_buf_size = 64,
	};
	int rc = ove_uart_create(&uart, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_uart_read_null_buf(void **state)
{
	(void)state;
	ove_uart_t uart;
	struct ove_uart_cfg cfg = {
		.instance = 0, .baudrate = 115200, .data_bits = 8,
		.parity = OVE_UART_PARITY_NONE, .stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE, .rx_buf_size = 64,
	};
	ove_uart_create(&uart, &cfg);

	int rc = ove_uart_read(uart, NULL, 4, 100, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_uart_destroy(uart);
}
#else /* CONFIG_OVE_ZERO_HEAP — use _init with caller-supplied storage */
OVE_UART_DEFINE(s_uart_cd, 64);
OVE_UART_DEFINE(s_uart_rd, 64);

static void test_uart_create_destroy(void **state)
{
	(void)state;
	ove_uart_t uart;
	struct ove_uart_cfg cfg = {
		.instance = 0, .baudrate = 115200, .data_bits = 8,
		.parity = OVE_UART_PARITY_NONE, .stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE, .rx_buf_size = 64,
	};
	int rc = ove_uart_init(&uart, &s_uart_cd, s_uart_cd_rx_buf, &cfg);
	assert_int_equal(rc, OVE_OK);
	ove_uart_deinit(uart);
}

static void test_uart_null_params(void **state)
{
	(void)state;
	struct ove_uart_cfg cfg = {
		.instance = 0, .baudrate = 115200, .data_bits = 8,
		.parity = OVE_UART_PARITY_NONE, .stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE, .rx_buf_size = 64,
	};
	int rc = ove_uart_init(NULL, &s_uart_cd, s_uart_cd_rx_buf, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_uart_zero_baudrate(void **state)
{
	(void)state;
	ove_uart_t uart;
	struct ove_uart_cfg cfg = {
		.instance = 0, .baudrate = 0, .data_bits = 8,
		.parity = OVE_UART_PARITY_NONE, .stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE, .rx_buf_size = 64,
	};
	int rc = ove_uart_init(&uart, &s_uart_cd, s_uart_cd_rx_buf, &cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_uart_read_null_buf(void **state)
{
	(void)state;
	ove_uart_t uart;
	struct ove_uart_cfg cfg = {
		.instance = 0, .baudrate = 115200, .data_bits = 8,
		.parity = OVE_UART_PARITY_NONE, .stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE, .rx_buf_size = 64,
	};
	ove_uart_init(&uart, &s_uart_rd, s_uart_rd_rx_buf, &cfg);

	int rc = ove_uart_read(uart, NULL, 4, 100, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_uart_deinit(uart);
}
#endif /* CONFIG_OVE_ZERO_HEAP */

static void test_uart_write_null_handle(void **state)
{
	(void)state;
	uint8_t data[] = "hello";
	int rc = ove_uart_write(NULL, data, sizeof(data), 100, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_uart_bytes_available_null(void **state)
{
	(void)state;
	size_t avail = ove_uart_bytes_available(NULL);
	assert_int_equal(avail, 0);
}

#else /* !CONFIG_OVE_UART */

static void test_uart_skipped(void **state)
{
	(void)state;
	skip();
}

#endif /* CONFIG_OVE_UART */

/* ── runner ──────────────────────────────────────────────────────────── */

int test_uart_run(void)
{
#ifdef CONFIG_OVE_UART
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_uart_create_destroy),
		cmocka_unit_test(test_uart_null_params),
		cmocka_unit_test(test_uart_zero_baudrate),
		cmocka_unit_test(test_uart_write_null_handle),
		cmocka_unit_test(test_uart_read_null_buf),
		cmocka_unit_test(test_uart_bytes_available_null),
	};
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_uart_skipped),
	};
#endif
	return cmocka_run_group_tests(tests, NULL, NULL);
}
