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
#include "stm32f7xx_hal.h"
#include <string.h>

static SPI_TypeDef *instance_to_periph(unsigned int instance)
{
	switch (instance) {
	case 0:
		return SPI1;
	case 1:
		return SPI2;
	case 2:
		return SPI3;
#ifdef SPI4
	case 3:
		return SPI4;
#endif
#ifdef SPI5
	case 4:
		return SPI5;
#endif
	default:
		return NULL;
	}
}

int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg)
{
	SPI_TypeDef *periph = instance_to_periph(cfg->instance);
	HAL_StatusTypeDef hal_ret;

	if (periph == NULL)
		return OVE_ERR_INVALID_PARAM;

	memset(&spi->hal_handle, 0, sizeof(spi->hal_handle));
	spi->hal_handle.Instance = periph;
	spi->hal_handle.Init.Mode = SPI_MODE_MASTER;
	spi->hal_handle.Init.Direction = SPI_DIRECTION_2LINES;
	spi->hal_handle.Init.DataSize = (cfg->word_size == 16) ? SPI_DATASIZE_16BIT
							       : SPI_DATASIZE_8BIT;
	spi->hal_handle.Init.NSS = SPI_NSS_SOFT;
	spi->hal_handle.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
	spi->hal_handle.Init.FirstBit = (cfg->bit_order == OVE_SPI_LSB_FIRST) ? SPI_FIRSTBIT_LSB
									      : SPI_FIRSTBIT_MSB;
	spi->hal_handle.Init.TIMode = SPI_TIMODE_DISABLE;
	spi->hal_handle.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;

	switch (cfg->mode) {
	case OVE_SPI_MODE_0:
		spi->hal_handle.Init.CLKPolarity = SPI_POLARITY_LOW;
		spi->hal_handle.Init.CLKPhase = SPI_PHASE_1EDGE;
		break;
	case OVE_SPI_MODE_1:
		spi->hal_handle.Init.CLKPolarity = SPI_POLARITY_LOW;
		spi->hal_handle.Init.CLKPhase = SPI_PHASE_2EDGE;
		break;
	case OVE_SPI_MODE_2:
		spi->hal_handle.Init.CLKPolarity = SPI_POLARITY_HIGH;
		spi->hal_handle.Init.CLKPhase = SPI_PHASE_1EDGE;
		break;
	case OVE_SPI_MODE_3:
		spi->hal_handle.Init.CLKPolarity = SPI_POLARITY_HIGH;
		spi->hal_handle.Init.CLKPhase = SPI_PHASE_2EDGE;
		break;
	}

	/* Prescaler: pick closest divider to requested clock_hz.
	 * STM32 SPI clock is derived from APB.  Use a conservative
	 * default prescaler — exact tuning requires knowing APBx freq. */
	spi->hal_handle.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;

	hal_ret = HAL_SPI_Init(&spi->hal_handle);
	if (hal_ret != HAL_OK)
		return OVE_ERR_NOT_SUPPORTED;

	return OVE_OK;
}

void ove_hal_spi_close(ove_spi_t spi)
{
	HAL_SPI_DeInit(&spi->hal_handle);
}

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint32_t timeout_ms)
{
	HAL_StatusTypeDef ret;

	if (tx != NULL && rx != NULL) {
		ret = HAL_SPI_TransmitReceive(&spi->hal_handle, (uint8_t *)tx, rx, (uint16_t)len,
					      timeout_ms);
	} else if (tx != NULL) {
		ret = HAL_SPI_Transmit(&spi->hal_handle, (uint8_t *)tx, (uint16_t)len, timeout_ms);
	} else {
		ret = HAL_SPI_Receive(&spi->hal_handle, rx, (uint16_t)len, timeout_ms);
	}

	switch (ret) {
	case HAL_OK:
		return OVE_OK;
	case HAL_TIMEOUT:
		return OVE_ERR_TIMEOUT;
	default:
		return OVE_ERR_BUS_ERROR;
	}
}

#endif /* CONFIG_OVE_SPI */
