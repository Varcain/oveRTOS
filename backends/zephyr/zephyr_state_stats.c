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
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_THREAD_STATE_STATS

#include <stdint.h>

#include <zephyr/kernel.h>

#include "ove/thread_state_stats.h"

uint64_t ove_state_stats_now_us(void)
{
	/* k_uptime_ticks + k_ticks_to_us_near64 is portable across all
	 * Zephyr arches (cycle_get_64 isn't guaranteed without
	 * CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) and gives tick-resolution
	 * monotonic microseconds. Safe to call from any context — both
	 * helpers are pure arithmetic / atomic tick reads. */
	return k_ticks_to_us_near64(k_uptime_ticks());
}

#endif /* CONFIG_OVE_THREAD_STATE_STATS */
