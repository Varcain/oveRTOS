/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/time.h"
#include "ove_backend_common.h"
#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

int ove_time_get_us(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	/* HAL_GetTick() returns milliseconds */
	*out = (uint64_t)HAL_GetTick() * 1000ULL;
	return OVE_OK;
}

int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	/* DWT cycle counter for nanosecond resolution */
	uint32_t cycles = DWT->CYCCNT;
	*out = (uint64_t)cycles * 1000000000ULL / (uint64_t)SystemCoreClock;
	return OVE_OK;
}

void ove_time_delay_ms(uint32_t ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

void ove_time_delay_us(uint32_t us)
{
	/* Busy-wait for microsecond delays (no RTOS support) */
	uint32_t start = DWT->CYCCNT;
	uint32_t cycles = (SystemCoreClock / 1000000U) * us;
	while ((DWT->CYCCNT - start) < cycles) {
		/* spin */
	}
}
