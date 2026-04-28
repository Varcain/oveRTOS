/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_SPI

#include "ove/hal/hal_spi.h"
#include "ove_backend_common.h"
#include <zephyr/drivers/spi.h>

static const struct device *instance_to_dev(unsigned int instance)
{
	switch (instance) {
	case 0:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi1));
	case 1:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi2));
	case 2:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi3));
	case 3:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi4));
	default:
		return NULL;
	}
}

int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg)
{
	const struct device *dev = instance_to_dev(cfg->instance);

	if (dev == NULL || !device_is_ready(dev))
		return OVE_ERR_INVALID_PARAM;

	spi->dev = dev;
	return OVE_OK;
}

void ove_hal_spi_close(ove_spi_t spi)
{
	spi->dev = NULL;
}

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint32_t timeout_ms)
{
	(void)timeout_ms;

	uint16_t operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(spi->word_size);

	switch (spi->mode) {
	case OVE_SPI_MODE_1:
		operation |= SPI_MODE_CPHA;
		break;
	case OVE_SPI_MODE_2:
		operation |= SPI_MODE_CPOL;
		break;
	case OVE_SPI_MODE_3:
		operation |= SPI_MODE_CPOL | SPI_MODE_CPHA;
		break;
	default:
		break;
	}

	if (spi->bit_order == OVE_SPI_LSB_FIRST)
		operation |= SPI_TRANSFER_LSB;

	struct spi_config config = {
		.frequency = spi->clock_hz,
		.operation = operation,
	};

	struct spi_buf tx_buf = {.buf = (void *)tx, .len = len};
	struct spi_buf rx_buf = {.buf = rx, .len = len};

	struct spi_buf_set tx_set = {.buffers = tx ? &tx_buf : NULL, .count = tx ? 1 : 0};
	struct spi_buf_set rx_set = {.buffers = rx ? &rx_buf : NULL, .count = rx ? 1 : 0};

	int ret = spi_transceive(spi->dev, &config, &tx_set, &rx_set);
	return (ret == 0) ? OVE_OK : OVE_ERR_BUS_ERROR;
}

#endif /* CONFIG_OVE_SPI */
