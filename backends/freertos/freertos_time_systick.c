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
#include "ove_freertos_systick.h"

extern uint32_t SystemCoreClock;

static uint64_t now_ns_last;

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
	TickType_t ticks;
	uint32_t elapsed;

	/* Pending-reload-safe snapshot (see ove_freertos_systick.h). Scale the
	 * tick count and the sub-tick cycles separately: converting the total
	 * cycle count, ticks * (LOAD + 1) * 1e9 overflowed 64 bits after
	 * ~738 s at 25 MHz. ticks * 1e9 fits for the whole 32-bit tick range. */
	ove_freertos_systick_read(&ticks, &elapsed);
	uint64_t ns = (uint64_t)ticks * 1000000000ULL / configTICK_RATE_HZ +
		      (uint64_t)elapsed * 1000000000ULL / (uint64_t)SystemCoreClock;
	*out = ove_freertos_monotonic(&now_ns_last, ns);
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
