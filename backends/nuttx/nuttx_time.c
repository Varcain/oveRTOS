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
	*out = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
	return OVE_OK;
}

#if (defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_7M__)) && !defined(CONFIG_SCHED_TICKLESS)
/*
 * ARM Cortex-M with a periodic SysTick tick: whole ticks from the NuttX tick
 * count (which also covers sleep/WFI time) plus sub-tick nanoseconds from the
 * SysTick down-counter, without DWT (which QEMU does not emulate). Cycles
 * per tick come from the SysTick reload register, so no board-specific
 * constants are needed.
 */
#define OVE_NX_SYSTICK 1
#include <stdbool.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include "ove_cortexm_systick.h"

static uint64_t now_ns_last;

int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	/*
	 * Must stay monotonic with interrupts masked and in handler mode,
	 * where the SysTick handler cannot run: a reload it has not counted
	 * yet shows up as PENDSTSET and adds one tick. CLOCK_MONOTONIC is the
	 * handler's own tick count; clock_systime_ticks() is not, as under
	 * CONFIG_TIMER_ARCH it already adds a pending reload itself.
	 */
	irqstate_t flags = up_irq_save();
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t load;
	bool pending;
	uint32_t cycles = ove_cortexm_systick_elapsed(&load, &pending);
	uint64_t ns = (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec +
		      (pending ? NSEC_PER_TICK : 0u) +
		      (uint64_t)cycles * NSEC_PER_TICK / ((uint64_t)load + 1u);

	/* Emulated SysTick can show a reload before PENDSTSET: hold such
	 * small steps back. A larger one is the 32-bit tick count wrapping:
	 * let it through rather than freeze. */
	if (ns < now_ns_last && now_ns_last - ns <= 2u * NSEC_PER_TICK)
		ns = now_ns_last;
	else
		now_ns_last = ns;
	up_irq_restore(flags);

	*out = ns;
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
	*out = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	return OVE_OK;
}
#endif

void ove_time_delay_ms(uint32_t ms)
{
	OVE_RETURN_IF_ISR();
#ifdef OVE_NX_SYSTICK
	/*
	 * For short delays (< one tick), busy-wait using SysTick to
	 * avoid NuttX tick-granularity rounding.  For longer delays,
	 * fall through to usleep() so other tasks can run.
	 */
	uint32_t usec_per_tick = 1000000U / TICK_PER_SEC;

	if (ms * 1000U < usec_per_tick) {
		uint32_t load = OVE_SYST_RVR;
		uint64_t freq = (uint64_t)(load + 1) * TICK_PER_SEC;
		uint32_t cycles = (uint32_t)(freq / 1000U) * ms;
		uint32_t start = OVE_SYST_CVR;

		while (1) {
			uint32_t now = OVE_SYST_CVR;
			uint32_t elapsed = (start >= now) ? (start - now)
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
#ifdef OVE_NX_SYSTICK
	uint32_t usec_per_tick = 1000000U / TICK_PER_SEC;

	if (us < usec_per_tick) {
		uint32_t load = OVE_SYST_RVR;
		uint64_t freq = (uint64_t)(load + 1) * TICK_PER_SEC;
		uint32_t cycles = (uint32_t)(freq / 1000000U) * us;
		uint32_t start = OVE_SYST_CVR;

		while (1) {
			uint32_t now = OVE_SYST_CVR;
			uint32_t elapsed = (start >= now) ? (start - now)
							  : (start + load + 1 - now);
			if (elapsed >= cycles)
				break;
		}
		return;
	}
#endif
	OVE_RETURN_IF_ISR();
	usleep(us);
}
