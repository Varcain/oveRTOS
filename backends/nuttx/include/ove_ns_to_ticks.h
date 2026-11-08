/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NS_TO_TICKS_NUTTX_H
#define OVE_NS_TO_TICKS_NUTTX_H

#include <stdint.h>
#include <nuttx/clock.h>
#include <nuttx/wdog.h>
#include "ove/types.h"

/**
 * @brief Convert uint64_t nanoseconds to NuttX clock_t ticks.
 *
 * Keep the bounded common path in 32-bit arithmetic. The slow path uses
 * quotient/remainder so rounding cannot overflow, and saturates at UINT32_MAX.
 * Timer admission uses that saturation to reject unsupported exact periods.
 *
 * Blocking budgets use ove_ns_to_wait_ticks instead.
 */
static inline clock_t ove_ns_to_ticks(uint64_t ns)
{
	const uint64_t period = NSEC_PER_TICK;

	if (period <= UINT32_MAX && ns <= (uint64_t)UINT32_MAX - (period - 1)) {
		uint32_t rounded = (uint32_t)ns + (uint32_t)(period - 1);
		return (clock_t)(rounded / (uint32_t)period);
	}

	uint64_t ticks = ns / period + (ns % period != 0);
	if (ticks > (uint64_t)UINT32_MAX) {
		ticks = UINT32_MAX;
	}
	return (clock_t)ticks;
}

/* Callers handle explicit FOREVER separately. Native uninterruptible waits
 * capture end = now + delay + 1; a same-tick EINTR can retry one tick larger.
 * Leave that headroom below watchdog rejection and the signed retry boundary. */
static inline uint32_t ove_ns_to_wait_ticks(uint64_t ns)
{
	const uint64_t watchdog_limit = (uint64_t)WDOG_MAX_DELAY - 2;
	const uint64_t retry_limit = (uint64_t)INT32_MAX - 1;
	const uint64_t limit = watchdog_limit < retry_limit ? watchdog_limit : retry_limit;
	uint64_t ticks = ove_ns_to_ticks(ns);
	return (uint32_t)(ticks > limit ? limit : ticks);
}

#endif /* OVE_NS_TO_TICKS_NUTTX_H */
