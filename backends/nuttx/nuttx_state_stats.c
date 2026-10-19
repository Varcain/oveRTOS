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
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_THREAD_STATE_STATS

#include <stdint.h>
#include <time.h>

#include "ove/thread_state_stats.h"

uint64_t ove_state_stats_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

#endif /* CONFIG_OVE_THREAD_STATE_STATS */
