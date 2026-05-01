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

	/* HAL_GetTick() is driven by SysTick and gets frozen when
	 * configUSE_TICKLESS_IDLE suppresses ticks across sleep, so any
	 * caller computing now-then deltas across an idle (e.g. ove_pm
	 * time-in-state stats) loses the suspended interval.
	 * xTaskGetTickCount() is the kernel's authoritative tick count and
	 * gets bumped by vTaskStepTick() on wake from tickless sleep, so
	 * deltas remain correct.  xTaskGetTickCountFromISR() is the
	 * ISR-safe variant — same value, callable either way. */
	TickType_t ticks = xPortIsInsideInterrupt() ? xTaskGetTickCountFromISR()
						    : xTaskGetTickCount();
	*out = (uint64_t)ticks * (1000000ULL / configTICK_RATE_HZ);
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
