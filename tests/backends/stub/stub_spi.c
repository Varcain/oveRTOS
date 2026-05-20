/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Stub SPI HAL for the stub test backend — no real controller.
 * Lets the public-API tests in tests/suites/test_spi.c exercise
 * create/destroy/null-param/word-size paths on a host with no
 * SPI hardware.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_SPI

#include "ove/hal/hal_spi.h"

#include <string.h>

int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg)
{
	(void)cfg;
	spi->fd = 1;
	return OVE_OK;
}

void ove_hal_spi_close(ove_spi_t spi)
{
	spi->fd = -1;
}

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint64_t timeout_ns)
{
	(void)spi;
	(void)timeout_ns;
	/* Software loopback for tests that need a deterministic RX. */
	if (rx != NULL && tx != NULL && len > 0)
		memcpy(rx, tx, len);
	else if (rx != NULL && len > 0)
		memset(rx, 0, len);
	return OVE_OK;
}

#ifdef CONFIG_OVE_ASYNC

#include <pthread.h>
#include "ove_backend_common.h"

static void *stub_spi_async_worker(void *arg)
{
	ove_spi_t spi = (ove_spi_t)arg;
	int result;

	result = ove_hal_spi_transfer(spi, spi->pending_tx, spi->pending_rx, spi->pending_len,
				      OVE_WAIT_FOREVER);
	ove_spi_async_complete(spi, result);
	return NULL;
}

int ove_hal_spi_transfer_async(ove_spi_t spi, const void *tx, void *rx, size_t len)
{
	pthread_t tid;
	pthread_attr_t attr;
	int rc;

	spi->pending_tx = tx;
	spi->pending_rx = rx;
	spi->pending_len = len;

	if (pthread_attr_init(&attr) != 0)
		return OVE_ERR_NO_MEMORY;
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	rc = pthread_create(&tid, &attr, stub_spi_async_worker, spi);
	pthread_attr_destroy(&attr);
	if (rc != 0)
		return OVE_ERR_NO_MEMORY;
	return OVE_OK;
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_SPI */
