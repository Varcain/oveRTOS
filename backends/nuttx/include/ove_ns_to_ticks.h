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
#include "ove/types.h"

/**
 * @brief Convert uint64_t nanoseconds to NuttX clock_t ticks.
 *
 * Used by queue/sync/eventgroup hot paths. The fast path handles inputs
 * up to ~4.29 seconds with a 32-bit UDIV — one cycle on Cortex-M7 — and
 * avoids libgcc's __aeabi_uldivmod for the typical call site.
 *
 * The slow path (timeouts > ~4 seconds) does a true 64-bit divide and
 * saturates at UINT32_MAX ticks.
 */
static inline clock_t ove_ns_to_ticks(uint64_t ns)
{
	const uint64_t plus = ns + (uint64_t)(NSEC_PER_TICK - 1);

	/* Fast path: rounded-up value fits in 32 bits.  GCC emits a
	 * 3-instruction magic-multiply or single-cycle UDIV here. */
	if (plus <= (uint64_t)UINT32_MAX) {
		return (clock_t)((uint32_t)plus / (uint32_t)NSEC_PER_TICK);
	}

	/* Slow path: library __aeabi_uldivmod call.  Reserved for
	 * timeouts > ~4 seconds; saturate at UINT32_MAX. */
	uint64_t ticks = plus / NSEC_PER_TICK;
	if (ticks > (uint64_t)UINT32_MAX) {
		ticks = UINT32_MAX;
	}
	return (clock_t)ticks;
}

#endif /* OVE_NS_TO_TICKS_NUTTX_H */
