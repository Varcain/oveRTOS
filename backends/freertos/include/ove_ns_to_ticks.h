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

/**
 * @brief Convert uint64_t nanoseconds to FreeRTOS TickType_t.
 *
 * Used by queue/sync/stream/eventgroup hot paths. The fast path handles
 * inputs up to ~4.29 seconds (any tick rate) with a 32-bit UDIV — one
 * cycle on Cortex-M7 — and avoids pulling in libgcc's __aeabi_uldivmod
 * for the typical call site.
 *
 * The slow path (timeouts > ~4.29 seconds) does a true 64-bit divide
 * and saturates one tick below portMAX_DELAY so the wait-forever
 * sentinel stays distinct from "very long finite wait."
 */
static inline TickType_t ove_ns_to_ticks(uint64_t ns)
{
	if (ns == OVE_WAIT_FOREVER) {
		return portMAX_DELAY;
	}
	if (ns == 0) {
		/* Poll / try-* fast path: 0 ns ⇒ 0 ticks (non-blocking).  The
		 * round-up divide below already yields 0 for ns==0, so this is
		 * correctness-neutral; it just skips the UDIV on every poll. */
		return 0;
	}

	const uint64_t nsec_per_tick = 1000000000ULL / configTICK_RATE_HZ;
	const uint64_t plus = ns + (nsec_per_tick - 1u);

	/* Fast path: rounded-up value fits in 32 bits.  GCC emits a
	 * 3-instruction magic-multiply or single-cycle UDIV here. */
	if (plus <= (uint64_t)UINT32_MAX) {
		return (TickType_t)((uint32_t)plus / (uint32_t)nsec_per_tick);
	}

	/* Slow path: library __aeabi_uldivmod call.  Reserved for
	 * timeouts > ~4 seconds; saturate one below portMAX_DELAY. */
	uint64_t ticks = plus / nsec_per_tick;
	if (ticks >= (uint64_t)portMAX_DELAY) {
		ticks = (uint64_t)portMAX_DELAY - 1u;
	}
	return (TickType_t)ticks;
}

#endif /* OVE_NS_TO_TICKS_FREERTOS_H */
