/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_UART

#include "ove/uart.h"
#include "ove/hal/hal_uart.h"
#include "ove/sync.h"
#include "ove/stream.h"
#include "ove_backend_common.h"

#include <string.h>

/* ── Lifecycle ───────────────────────────────────────────────────── */

int ove_uart_init(ove_uart_t *uart, ove_uart_storage_t *storage, void *rx_buf,
		  const struct ove_uart_cfg *cfg)
{
	int ret;

	if (uart == NULL || storage == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (rx_buf == NULL || cfg->rx_buf_size == 0)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->baudrate == 0)
		return OVE_ERR_INVALID_PARAM;

	storage->instance = cfg->instance;
	storage->baudrate = cfg->baudrate;
	storage->rx_buf = rx_buf;
	storage->rx_buf_size = cfg->rx_buf_size;
	*uart = storage;

	/* Initialise the RX stream (trigger = 1 byte) */
	ret = ove_stream_init(&storage->rx_stream, &storage->rx_stream_storage, rx_buf,
			      cfg->rx_buf_size, 1);
	if (ret != OVE_OK)
		return ret;

	/* Initialise the TX mutex */
	ret = ove_mutex_init(&storage->tx_mtx, &storage->tx_mtx_storage);
	if (ret != OVE_OK) {
		ove_stream_deinit(storage->rx_stream);
		return ret;
	}

	/* Open the hardware peripheral and enable RX */
	ret = ove_hal_uart_open(*uart, cfg);
	if (ret != OVE_OK) {
		ove_mutex_deinit(storage->tx_mtx);
		ove_stream_deinit(storage->rx_stream);
		return ret;
	}

	ret = ove_hal_uart_rx_enable(*uart);
	if (ret != OVE_OK) {
		ove_hal_uart_close(*uart);
		ove_mutex_deinit(storage->tx_mtx);
		ove_stream_deinit(storage->rx_stream);
		return ret;
	}

	return OVE_OK;
}

void ove_uart_deinit(ove_uart_t uart)
{
	if (uart == NULL)
		return;
	ove_hal_uart_close(uart);
	ove_mutex_deinit(uart->tx_mtx);
	ove_stream_deinit(uart->rx_stream);
}

#ifdef OVE_HEAP_UART
int ove_uart_create(ove_uart_t *uart, const struct ove_uart_cfg *cfg)
{
	ove_uart_storage_t *storage;
	uint8_t *rx_buf;

	if (uart == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->rx_buf_size == 0)
		return OVE_ERR_INVALID_PARAM;

	storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (storage == NULL)
		return OVE_ERR_NO_MEMORY;

	rx_buf = OVE_BACKEND_MALLOC(cfg->rx_buf_size);
	if (rx_buf == NULL) {
		OVE_BACKEND_FREE(storage);
		return OVE_ERR_NO_MEMORY;
	}

	memset(storage, 0, sizeof(*storage));

	int ret = ove_uart_init(uart, storage, rx_buf, cfg);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(rx_buf);
		OVE_BACKEND_FREE(storage);
		return ret;
	}
	return OVE_OK;
}

void ove_uart_destroy(ove_uart_t uart)
{
	uint8_t *rx_buf;

	if (uart == NULL)
		return;

	rx_buf = uart->rx_buf;
	ove_uart_deinit(uart);
	OVE_BACKEND_FREE(rx_buf);
	OVE_BACKEND_FREE(uart);
}
#endif /* OVE_HEAP_UART */

/* ── Operations ──────────────────────────────────────────────────── */

int ove_uart_write(ove_uart_t uart, const void *data, size_t len, uint64_t timeout_ns,
		   size_t *bytes_written)
{
	int ret;

	if (uart == NULL || (data == NULL && len > 0))
		return OVE_ERR_INVALID_PARAM;

	OVE_LOCK_INFINITE(uart->tx_mtx);
	ret = ove_hal_uart_tx(uart, data, len, timeout_ns, bytes_written);
	ove_mutex_unlock(uart->tx_mtx);

	return ret;
}

int ove_uart_read(ove_uart_t uart, void *buf, size_t len, uint64_t timeout_ns, size_t *bytes_read)
{
	if (uart == NULL || buf == NULL || len == 0)
		return OVE_ERR_INVALID_PARAM;

	return ove_stream_receive(uart->rx_stream, buf, len, timeout_ns, bytes_read);
}

size_t ove_uart_bytes_available(ove_uart_t uart)
{
	if (uart == NULL)
		return 0;
	return ove_stream_bytes_available(uart->rx_stream);
}

int ove_uart_flush(ove_uart_t uart)
{
	if (uart == NULL)
		return OVE_ERR_INVALID_PARAM;
	return ove_hal_uart_tx_flush(uart);
}

/* ── ISR helper ──────────────────────────────────────────────────── */

void ove_uart_rx_isr_push(ove_uart_t uart, const void *data, size_t len)
{
	if (uart == NULL || data == NULL || len == 0)
		return;
	ove_stream_send_from_isr(uart->rx_stream, data, len, NULL);
}

int ove_uart_set_rx_notify(ove_uart_t uart, ove_notify_cb cb, void *user_data)
{
	if (uart == NULL)
		return OVE_ERR_INVALID_PARAM;
	return ove_stream_set_notify(uart->rx_stream, cb, user_data);
}

#endif /* CONFIG_OVE_UART */
