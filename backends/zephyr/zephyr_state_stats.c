/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Zephyr time source for per-thread state tracking
 * (CONFIG_OVE_THREAD_STATE_STATS), shared with the trace stream and the
 * sampling profiler (both depend on this option).
 *
 * Uses the hardware cycle counter rather than kernel ticks: tick
 * resolution (100 us at 10 kHz) collapsed many state spans to zero length.
 * With a 64-bit cycle counter (prj.conf enables the SysTick driver's on the
 * QEMU boards) the conversion is direct; otherwise the 32-bit counter is
 * extended in software, which needs a call at least once per wrap
 * (~171 s at 25 MHz) — state transitions provide that in practice.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_THREAD_STATE_STATS

#include <stdint.h>

#include <zephyr/kernel.h>

#include "ove/thread_state_stats.h"

static uint64_t now_us_last;
#ifndef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
static uint32_t cyc32_last;
static uint64_t cyc32_high;
#endif

uint64_t ove_state_stats_now_us(void)
{
	unsigned int key = irq_lock();

#ifdef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
	uint64_t cycles = k_cycle_get_64();
#else
	uint32_t c = k_cycle_get_32();
	if (c < cyc32_last)
		cyc32_high += (uint64_t)1 << 32;
	cyc32_last = c;
	uint64_t cycles = cyc32_high | c;
#endif
	uint64_t us = k_cyc_to_us_floor64(cycles);

	/* Emulated SysTick (QEMU) is not perfectly monotonic around a reload
	 * (see backends/common/ove_cortexm_systick.h): never step back. */
	if (us < now_us_last)
		us = now_us_last;
	else
		now_us_last = us;

	irq_unlock(key);
	return us;
}

#endif /* CONFIG_OVE_THREAD_STATE_STATS */
