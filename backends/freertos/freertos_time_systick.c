/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Time backend for FreeRTOS targets without a running DWT cycle counter
 * (QEMU, Renode): the kernel tick count, refined below one tick by the
 * SysTick down-counter. SysTick stops during WFI, so on these targets a
 * nanosecond delta across idle time under-reports wall-clock time.
 */

#include "ove/time.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
#include "task.h"

#define SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)
#define SYSTICK_VAL (*(volatile uint32_t *)0xE000E018u)

extern uint32_t SystemCoreClock;

static TickType_t tick_count(void)
{
	return xPortIsInsideInterrupt() ? xTaskGetTickCountFromISR() : xTaskGetTickCount();
}

int ove_time_get_us(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	*out = (uint64_t)tick_count() * (1000000ULL / configTICK_RATE_HZ);
	return OVE_OK;
}

int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	uint32_t load = SYSTICK_LOAD;
	TickType_t ticks;
	uint32_t val;

	/* Retry when a tick lands between the two reads. */
	do {
		ticks = tick_count();
		val = SYSTICK_VAL;
	} while (ticks != tick_count());

	uint64_t cycles = (uint64_t)ticks * (load + 1) + (load - val);
	*out = cycles * 1000000000ULL / (uint64_t)SystemCoreClock;
	return OVE_OK;
}

void ove_time_delay_ms(uint32_t ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

void ove_time_delay_us(uint32_t us)
{
	/* No cycle counter to spin on: round up to whole milliseconds. */
	uint32_t ms = (us + 999) / 1000;
	if (ms == 0) {
		ms = 1;
	}
	vTaskDelay(pdMS_TO_TICKS(ms));
}
