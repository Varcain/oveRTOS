/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * NuttX time source for per-thread state tracking
 * (CONFIG_OVE_THREAD_STATE_STATS), shared with the trace stream and the
 * sampling profiler (both depend on this option).
 *
 * CLOCK_MONOTONIC only has system-tick resolution (1 ms here), which
 * collapsed about half of all state spans to zero length. On Cortex-M with
 * a SysTick-driven tick the time is refined below one tick from the SysTick
 * down-counter (DWT cycle counting, up_perf_gettime(), is not emulated by
 * QEMU). Cycles per tick come from SysTick's own reload register.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_THREAD_STATE_STATS

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>

#include "ove/thread_state_stats.h"

#if (defined(CONFIG_ARCH_ARMV7M) || defined(CONFIG_ARCH_ARMV8M)) && !defined(CONFIG_SCHED_TICKLESS)
#define OVE_NX_SYSTICK_REFINE 1
#include "ove_cortexm_systick.h"
#endif

static uint64_t tick_clock_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

#ifdef OVE_NX_SYSTICK_REFINE
static uint64_t now_us_last;
/* Q32 cycles->us factor for the current SysTick reload (division-free hot
 * path; recomputed only when the reload changes). */
static uint32_t q32_load;
static uint32_t q32_us_per_cycle;

uint64_t ove_state_stats_now_us(void)
{
	if (!ove_cortexm_systick_running())
		return tick_clock_us();

	/* With interrupts masked the tick count cannot advance; a reload
	 * that happened meanwhile shows up as PENDSTSET. */
	irqstate_t flags = enter_critical_section();
	clock_t ticks = clock_systime_ticks();
	uint32_t load;
	bool pending;
	uint32_t cycles = ove_cortexm_systick_elapsed(&load, &pending);
	if (pending)
		ticks++;

	uint64_t sub_us;
	if ((uint64_t)load + 1u > USEC_PER_TICK) {
		if (load != q32_load) {
			q32_us_per_cycle = ove_q32_factor(USEC_PER_TICK, load + 1u);
			q32_load = load;
		}
		sub_us = ove_q32_apply(cycles, q32_us_per_cycle);
	} else {
		sub_us = (uint64_t)cycles * USEC_PER_TICK / ((uint64_t)load + 1u);
	}
	uint64_t us = (uint64_t)ticks * USEC_PER_TICK + sub_us;

	/* Emulated SysTick can show a reload before PENDSTSET (see
	 * ove_cortexm_systick.h): never step back. */
	if (us < now_us_last)
		us = now_us_last;
	else
		now_us_last = us;
	leave_critical_section(flags);
	return us;
}
#else
uint64_t ove_state_stats_now_us(void)
{
	return tick_clock_us();
}
#endif

#endif /* CONFIG_OVE_THREAD_STATE_STATS */
