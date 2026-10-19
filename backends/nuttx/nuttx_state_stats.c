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
	return (uint64_t)ts.tv_sec * 1000000u + (uint32_t)ts.tv_nsec / 1000u;
}

#ifdef OVE_NX_SYSTICK_REFINE
static uint64_t now_us_last;

uint64_t ove_state_stats_now_us(void)
{
	if (!ove_cortexm_systick_running())
		return tick_clock_us();

	/* CLOCK_MONOTONIC counts the ticks the SysTick handler has run
	 * (g_system_ticks, with or without CONFIG_TIMER_ARCH). With
	 * interrupts masked that count cannot advance; a reload it has not
	 * counted yet shows up as PENDSTSET. (clock_systime_ticks() does not
	 * work here: under CONFIG_TIMER_ARCH it adds a pending reload itself,
	 * from a PENDSTSET read of its own that cannot be matched with ours.) */
	irqstate_t flags = enter_critical_section();
	uint64_t us = tick_clock_us();
	uint32_t load;
	bool pending;
	uint32_t cycles = ove_cortexm_systick_elapsed(&load, &pending);
	if (pending)
		us += USEC_PER_TICK;
	us += (uint64_t)cycles * USEC_PER_TICK / ((uint64_t)load + 1u);

	/* Emulated SysTick can show a reload before PENDSTSET (see
	 * backends/freertos/include/ove_freertos_systick.h): hold such small
	 * steps back. A larger one is the 32-bit tick count wrapping (49.7
	 * days at 1 ms): let it through rather than freeze. */
	if (us < now_us_last && now_us_last - us <= 2u * USEC_PER_TICK)
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
