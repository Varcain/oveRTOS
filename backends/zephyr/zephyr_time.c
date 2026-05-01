/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/time.h"
#include "ove_backend_common.h"
#include <zephyr/kernel.h>

/*
 * k_cycle_get_32() is a 32-bit cycle counter.  At 216 MHz it wraps every
 * ~19.9 s — feeding it into k_cyc_to_us_floor64() does NOT extend the
 * range, so subtracting two samples taken across a wrap underflows
 * uint64 and corrupts long-running deltas (e.g. PM time-in-state stats).
 *
 * Use the kernel's 64-bit tick counter instead — it's the same source
 * Zephyr's own k_uptime_get() is built on, and it cannot wrap within any
 * realistic device lifetime.
 */
int ove_time_get_us(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	*out = (uint64_t)k_ticks_to_us_floor64(sys_clock_tick_get());
	return OVE_OK;
}

int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	*out = (uint64_t)k_ticks_to_ns_floor64(sys_clock_tick_get());
	return OVE_OK;
}

void ove_time_delay_ms(uint32_t ms)
{
	k_sleep(K_MSEC(ms));
}

void ove_time_delay_us(uint32_t us)
{
	k_busy_wait(us);
}
