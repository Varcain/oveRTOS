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

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx,
			 size_t len, uint32_t timeout_ms)
{
	(void)spi; (void)tx; (void)timeout_ms;
	if (rx != NULL && len > 0)
		memset(rx, 0, len);
	return OVE_OK;
}

#endif /* CONFIG_OVE_SPI */
