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
#include "stm32f7_init.h"
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
		return OVE_ERR_IO;

	return OVE_OK;
}

void ove_hal_spi_close(ove_spi_t spi)
{
	HAL_SPI_DeInit(&spi->hal_handle);
}

int ove_hal_spi_validate_transfer(ove_spi_t spi, size_t len, int asynchronous)
{
	size_t width = spi->word_size == 16 ? 2 : 1;
	if (asynchronous && len / width > UINT16_MAX)
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint64_t timeout_ns)
{
	const uint8_t *txp = tx;
	uint8_t *rxp = rx;
	size_t width = spi->word_size == 16 ? 2 : 1;
	size_t words = len / width;
	uint32_t budget = stm32f7_ns_to_hal_ms(timeout_ns);
	uint32_t started = HAL_GetTick();
	int attempted = 0;

	while (words) {
		uint16_t count = words > UINT16_MAX ? UINT16_MAX : (uint16_t)words;
		uint32_t remaining = budget;
		if (budget != HAL_MAX_DELAY) {
			uint32_t elapsed = HAL_GetTick() - started;
			if (elapsed >= budget && (attempted || elapsed))
				return OVE_ERR_TIMEOUT;
			remaining = budget - elapsed;
		}
		attempted = 1;
		HAL_StatusTypeDef ret;
		if (txp && rxp)
			ret = HAL_SPI_TransmitReceive(&spi->hal_handle, (uint8_t *)txp, rxp, count,
						      remaining);
		else if (txp)
			ret = HAL_SPI_Transmit(&spi->hal_handle, (uint8_t *)txp, count, remaining);
		else
			ret = HAL_SPI_Receive(&spi->hal_handle, rxp, count, remaining);
		if (ret == HAL_TIMEOUT)
			return OVE_ERR_TIMEOUT;
		if (ret == HAL_BUSY)
			return OVE_ERR_BUS_BUSY;
		if (ret != HAL_OK)
			return OVE_ERR_BUS_ERROR;
		size_t bytes = (size_t)count * width;
		if (txp)
			txp += bytes;
		if (rxp)
			rxp += bytes;
		words -= count;
	}
	return OVE_OK;
}

#ifdef CONFIG_OVE_ASYNC

/*
 * IT-mode async transfer. Uses HAL_SPI_TransmitReceive_IT (and the
 * write-only / read-only variants); completion fires
 * HAL_SPI_TxRxCpltCallback in the SPI ISR which dispatches to
 * ove_spi_async_complete.
 *
 * DMA mode would be faster but needs per-board DMA-stream wiring at
 * board init time (cache invalidate for D-cached RX); IT mode works
 * out of the box and gives us the async semantics. Boards that need
 * higher throughput can override HAL_SPI_MspInit to register DMA
 * handles and swap the _IT variants below for _DMA.
 */

/* Map a HAL SPI handle back to our ove_spi storage so the completion
 * callback knows which handle to notify. We don't have a per-instance
 * userdata pointer in the HAL handle, so look up by SPI_TypeDef. */
static struct ove_spi *s_active_spi[5];

static void register_active(struct ove_spi *spi)
{
	unsigned int idx = spi->instance;
	if (idx < (sizeof(s_active_spi) / sizeof(s_active_spi[0])))
		s_active_spi[idx] = spi;
}

static struct ove_spi *find_active(SPI_HandleTypeDef *hspi)
{
	for (unsigned int i = 0; i < sizeof(s_active_spi) / sizeof(s_active_spi[0]); i++) {
		if (s_active_spi[i] != NULL && &s_active_spi[i]->hal_handle == hspi)
			return s_active_spi[i];
	}
	return NULL;
}

int ove_hal_spi_transfer_async(ove_spi_t spi, const void *tx, void *rx, size_t len)
{
	HAL_StatusTypeDef ret;
	int valid = ove_hal_spi_validate_transfer(spi, len, 1);
	if (valid != OVE_OK)
		return valid;
	uint16_t words = (uint16_t)(len / (spi->word_size == 16 ? 2 : 1));

	register_active(spi);

	if (tx != NULL && rx != NULL) {
		ret = HAL_SPI_TransmitReceive_IT(&spi->hal_handle, (uint8_t *)tx, rx, words);
	} else if (tx != NULL) {
		ret = HAL_SPI_Transmit_IT(&spi->hal_handle, (uint8_t *)tx, words);
	} else {
		ret = HAL_SPI_Receive_IT(&spi->hal_handle, rx, words);
	}

	if (ret != HAL_OK) {
		s_active_spi[spi->instance] = NULL;
		return (ret == HAL_BUSY) ? OVE_ERR_BUS_BUSY : OVE_ERR_BUS_ERROR;
	}
	return OVE_OK;
}

static void async_done(SPI_HandleTypeDef *hspi, int result)
{
	struct ove_spi *spi = find_active(hspi);
	if (spi == NULL)
		return;
	s_active_spi[spi->instance] = NULL;
	ove_spi_async_complete(spi, result);
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
	async_done(hspi, OVE_OK);
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
	async_done(hspi, OVE_OK);
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
	async_done(hspi, OVE_OK);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
	async_done(hspi, OVE_ERR_BUS_ERROR);
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_SPI */
