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
 * The SysTick driver keeps a 64-bit cycle count by default above 60 MHz
 * (STM32F746) and prj.conf enables it on the 25 MHz QEMU boards. Without
 * one, fall back to kernel ticks: extending the 32-bit count in software
 * would need a call at least once per wrap, and would turn a small
 * backward step into a 2^32-cycle jump forward.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_THREAD_STATE_STATS

#include <stdint.h>

#include <zephyr/kernel.h>

#include "ove/thread_state_stats.h"

#ifdef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
static uint64_t now_us_last;

uint64_t ove_state_stats_now_us(void)
{
	unsigned int key = irq_lock();
	uint64_t us = k_cyc_to_us_floor64(k_cycle_get_64());

	/* Emulated SysTick (QEMU) is not perfectly monotonic around a reload
	 * (see backends/freertos/include/ove_freertos_systick.h): never step
	 * back. */
	if (us < now_us_last)
		us = now_us_last;
	else
		now_us_last = us;

	irq_unlock(key);
	return us;
}
#else
uint64_t ove_state_stats_now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}
#endif

#endif /* CONFIG_OVE_THREAD_STATE_STATS */
