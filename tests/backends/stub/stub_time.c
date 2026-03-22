/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stub time backend for bare-metal testing (QEMU).
 * Uses FreeRTOS tick count instead of POSIX clock_gettime.
 */

#include "ove/ove.h"

#ifdef OVE_QEMU_ARM
#include "FreeRTOS.h"
#include "task.h"

/* SysTick registers (always available on Cortex-M, emulated by QEMU) */
#define STUB_SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014)
#define STUB_SYSTICK_VAL  (*(volatile uint32_t *)0xE000E018)

extern uint32_t SystemCoreClock;

int ove_time_get_us(uint64_t *out)
{
	if (!out) {
		return OVE_ERR_INVALID_PARAM;
	}
	TickType_t ticks = xTaskGetTickCount();
	*out = (uint64_t)ticks * (1000000ULL / configTICK_RATE_HZ);
	return OVE_OK;
}

int ove_time_get_ns(uint64_t *out)
{
	if (!out) {
		return OVE_ERR_INVALID_PARAM;
	}
	/*
	 * SysTick counts DOWN from LOAD to 0, then reloads.
	 * Combined with tick count, this gives sub-tick precision
	 * for active-CPU measurements.
	 *
	 * Limitation: SysTick stops during WFI on Cortex-M, so sleep/delay
	 * benchmarks will under-report wall-clock time on QEMU.
	 */
	uint32_t load = STUB_SYSTICK_LOAD;
	TickType_t ticks, ticks2;
	uint32_t val;

	do {
		ticks = xTaskGetTickCount();
		val = STUB_SYSTICK_VAL;
		ticks2 = xTaskGetTickCount();
	} while (ticks != ticks2);

	uint32_t elapsed_in_tick = load - val;
	uint64_t total_cycles = (uint64_t)ticks * (load + 1) + elapsed_in_tick;
	*out = total_cycles * 1000000000ULL / (uint64_t)SystemCoreClock;
	return OVE_OK;
}

void ove_time_delay_ms(uint32_t ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

void ove_time_delay_us(uint32_t us)
{
	uint32_t ms = (us + 999) / 1000;
	if (ms == 0) {
		ms = 1;
	}
	vTaskDelay(pdMS_TO_TICKS(ms));
}

#else
/* Native/POSIX fallback using clock_gettime */
#include <time.h>
#include <errno.h>

int ove_time_get_us(uint64_t *out)
{
	struct timespec ts;
	if (!out) {
		return OVE_ERR_INVALID_PARAM;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)ts.tv_nsec / 1000ULL;
	return OVE_OK;
}

int ove_time_get_ns(uint64_t *out)
{
	struct timespec ts;
	if (!out) {
		return OVE_ERR_INVALID_PARAM;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (uint64_t)ts.tv_sec * 1000000000ULL +
	       (uint64_t)ts.tv_nsec;
	return OVE_OK;
}

static void delay_ns(uint64_t ns)
{
	struct timespec now;
	struct timespec target;

	clock_gettime(CLOCK_MONOTONIC, &now);
	target.tv_sec = now.tv_sec + (time_t)(ns / 1000000000ULL);
	target.tv_nsec = now.tv_nsec + (long)(ns % 1000000000ULL);
	if (target.tv_nsec >= 1000000000L) {
		target.tv_sec++;
		target.tv_nsec -= 1000000000L;
	}

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			       &target, NULL) == EINTR) {
	}
}

void ove_time_delay_ms(uint32_t ms)
{
	delay_ns((uint64_t)ms * 1000000ULL);
}

void ove_time_delay_us(uint32_t us)
{
	delay_ns((uint64_t)us * 1000ULL);
}
#endif
