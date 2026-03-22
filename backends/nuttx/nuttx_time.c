/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/time.h"
#include "ove_backend_common.h"
#include <time.h>
#include <unistd.h>

int ove_time_get_us(uint64_t *out)
{
	struct timespec ts;

	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)ts.tv_nsec / 1000ULL;
	return OVE_OK;
}

#if defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_7M__)
/*
 * ARM Cortex-M: use SysTick for sub-tick nanosecond precision.
 * SysTick counts DOWN from LOAD to 0 each tick. Combined with the
 * NuttX tick count, this gives cycle-level timestamps without
 * requiring DWT (which QEMU does not emulate).
 *
 * Clock frequency is derived from the LOAD register and NuttX tick rate
 * so no board-specific constants are needed.
 */
#include <nuttx/clock.h>

#define NUTTX_SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014)
#define NUTTX_SYSTICK_VAL  (*(volatile uint32_t *)0xE000E018)

int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	/*
	 * Hybrid approach: tick count for whole-tick ns (captures sleep/WFI
	 * time correctly) + SysTick VAL for sub-tick ns precision.
	 */
	uint32_t load = NUTTX_SYSTICK_LOAD;
	clock_t ticks, ticks2;
	uint32_t val;

	/* Double-read to avoid race when SysTick wraps between reads */
	do {
		ticks = clock_systime_ticks();
		val = NUTTX_SYSTICK_VAL;
		ticks2 = clock_systime_ticks();
	} while (ticks != ticks2);

	uint64_t tick_ns = (uint64_t)ticks *
			   (1000000000ULL / TICK_PER_SEC);
	uint32_t elapsed_in_tick = load - val;
	uint64_t freq = (uint64_t)(load + 1) * TICK_PER_SEC;
	uint64_t sub_tick_ns = (uint64_t)elapsed_in_tick *
			       1000000000ULL / freq;

	*out = tick_ns + sub_tick_ns;
	return OVE_OK;
}
#else
int ove_time_get_ns(uint64_t *out)
{
	struct timespec ts;

	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (uint64_t)ts.tv_sec * 1000000000ULL +
	       (uint64_t)ts.tv_nsec;
	return OVE_OK;
}
#endif

void ove_time_delay_ms(uint32_t ms)
{
#if defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_7M__)
	/*
	 * For short delays (< one tick), busy-wait using SysTick to
	 * avoid NuttX tick-granularity rounding.  For longer delays,
	 * fall through to usleep() so other tasks can run.
	 */
	uint32_t usec_per_tick = 1000000U / TICK_PER_SEC;

	if (ms * 1000U < usec_per_tick) {
		uint32_t load = NUTTX_SYSTICK_LOAD;
		uint64_t freq = (uint64_t)(load + 1) * TICK_PER_SEC;
		uint32_t cycles = (uint32_t)(freq / 1000U) * ms;
		uint32_t start = NUTTX_SYSTICK_VAL;

		while (1) {
			uint32_t now = NUTTX_SYSTICK_VAL;
			uint32_t elapsed = (start >= now)
				? (start - now)
				: (start + load + 1 - now);
			if (elapsed >= cycles)
				break;
		}
		return;
	}
#endif
	usleep(ms * 1000U);
}

void ove_time_delay_us(uint32_t us)
{
#if defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_7M__)
	uint32_t usec_per_tick = 1000000U / TICK_PER_SEC;

	if (us < usec_per_tick) {
		uint32_t load = NUTTX_SYSTICK_LOAD;
		uint64_t freq = (uint64_t)(load + 1) * TICK_PER_SEC;
		uint32_t cycles = (uint32_t)(freq / 1000000U) * us;
		uint32_t start = NUTTX_SYSTICK_VAL;

		while (1) {
			uint32_t now = NUTTX_SYSTICK_VAL;
			uint32_t elapsed = (start >= now)
				? (start - now)
				: (start + load + 1 - now);
			if (elapsed >= cycles)
				break;
		}
		return;
	}
#endif
	usleep(us);
}
