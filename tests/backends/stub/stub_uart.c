/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Stub UART HAL for the stub test backend — no real device, no RX
 * thread.  Lets the public-API tests in tests/suites/test_uart.c
 * exercise create/destroy/null-param paths on a host with no UART
 * hardware.  No RX bytes are ever pushed into the portable layer's
 * stream, which matches the tests' expectation of a quiet line.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_UART

#include "ove/hal/hal_uart.h"

int ove_hal_uart_open(ove_uart_t uart, const struct ove_uart_cfg *cfg)
{
	(void)cfg;
	uart->fd = 1;
	return OVE_OK;
}

void ove_hal_uart_close(ove_uart_t uart)
{
	uart->fd = -1;
}

int ove_hal_uart_tx(ove_uart_t uart, const void *data, size_t len, uint32_t timeout_ms,
		    size_t *bytes_written)
{
	(void)uart;
	(void)data;
	(void)timeout_ms;
	if (bytes_written != NULL)
		*bytes_written = len;
	return OVE_OK;
}

int ove_hal_uart_rx_enable(ove_uart_t uart)
{
	(void)uart;
	return OVE_OK;
}

int ove_hal_uart_tx_flush(ove_uart_t uart)
{
	(void)uart;
	return OVE_OK;
}

#endif /* CONFIG_OVE_UART */
