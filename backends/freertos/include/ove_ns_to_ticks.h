/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NS_TO_TICKS_FREERTOS_H
#define OVE_NS_TO_TICKS_FREERTOS_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "ove/types.h"

/* The native scheduler requires a positive effective rate. Diagnose invalid
 * zero configurations before a caller could execute a division. */
OVE_STATIC_ASSERT((uint64_t)configTICK_RATE_HZ > 0, "FreeRTOS tick rate must be positive");

/**
 * @brief Convert encoded nanoseconds to FreeRTOS ticks.
 *
 * Positive finite values use exact ceil(ns * Hz / 1e9), saturating one tick
 * below portMAX_DELAY before narrowing. Integral-period rates retain a bounded
 * 32-bit numerator/division path. No overflowing product or rounded addition
 * is needed for the general rational conversion.
 */
static inline TickType_t ove_ns_to_ticks(uint64_t ns)
{
	if (ns == OVE_WAIT_FOREVER) {
		return portMAX_DELAY;
	}
	if (ns == 0) {
		return 0;
	}

	const uint64_t scale = UINT64_C(1000000000);
	const uint64_t rate = (uint64_t)configTICK_RATE_HZ;
	const uint64_t limit = (uint64_t)portMAX_DELAY - 1;
	/* Rates above the scale use only the general path. Keep their unused
	 * specialization divisor nonzero so its expressions stay well formed. */
	const uint32_t period = rate <= scale ? (uint32_t)(scale / rate) : 1;
	if (scale % rate == 0) {
		uint64_t ticks;
		if (ns <= (uint64_t)UINT32_MAX - (period - 1u)) {
			uint32_t rounded = (uint32_t)ns + (period - 1u);
			ticks = rounded / period;
		} else {
			ticks = ns / period + (ns % period != 0);
		}
		return (TickType_t)(ticks > limit ? limit : ticks);
	}

	/* H = whole * scale + part, ns = seconds * scale + subns:
	 * ceil(ns * H / scale) = ns * whole + seconds * part
	 *                       + ceil(subns * part / scale).
	 * Admit each nonnegative term against the remaining finite allowance. */
	const uint64_t whole = rate / scale;
	const uint64_t part = rate % scale;
	if (whole != 0 && whole > limit / ns) {
		return (TickType_t)limit;
	}
	uint64_t ticks = ns * whole;
	uint64_t remaining = limit - ticks;
	const uint64_t seconds = ns / scale;
	if (part != 0 && seconds != 0 && part > remaining / seconds) {
		return (TickType_t)limit;
	}
	uint64_t second_term = seconds * part;
	ticks += second_term;
	remaining -= second_term;

	/* Both factors are below scale, so this product is below 1e18. */
	uint64_t numerator = (ns % scale) * part;
	uint64_t tail = numerator / scale + (numerator % scale != 0);
	if (tail > remaining) {
		return (TickType_t)limit;
	}
	return (TickType_t)(ticks + tail);
}

#endif /* OVE_NS_TO_TICKS_FREERTOS_H */
