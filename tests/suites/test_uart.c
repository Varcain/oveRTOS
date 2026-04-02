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

static void test_uart_write_null_handle(void **state)
{
	(void)state;
	uint8_t data[] = "hello";
	int rc = ove_uart_write(NULL, data, sizeof(data), 100, NULL);
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

static void test_uart_bytes_available_null(void **state)
{
	(void)state;
	size_t avail = ove_uart_bytes_available(NULL);
	assert_int_equal(avail, 0);
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
	return cmocka_run_group_tests(tests, NULL, NULL);
#else
	return 0;
#endif
}
