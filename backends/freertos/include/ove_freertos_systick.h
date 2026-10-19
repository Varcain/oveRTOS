/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Consistent (tick count, sub-tick cycles) snapshot from the FreeRTOS tick
 * count and the Cortex-M SysTick down-counter, callable from task, ISR,
 * PendSV and critical-section context. The pending-reload handling lives
 * in backends/common/ove_cortexm_systick.h.
 *
 * Limitation: with configUSE_TICKLESS_IDLE the port reprograms LOAD while
 * ticks are suppressed; snapshots taken from ISRs during such a sleep are
 * approximate (the kernel steps the tick count on wake-up).
 */

#ifndef OVE_FREERTOS_SYSTICK_H
#define OVE_FREERTOS_SYSTICK_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ove_cortexm_systick.h"

/**
 * @brief Read the tick count and the SysTick cycles elapsed since that tick.
 *
 * @param[out] ticks    Kernel ticks, including a reload whose handler is
 *                      still pending.
 * @param[out] elapsed  SysTick cycles since the start of tick @p ticks
 *                      (0 .. LOAD).
 */
static inline void ove_freertos_systick_read(TickType_t *ticks, uint32_t *elapsed)
{
	const int in_isr = xPortIsInsideInterrupt() != pdFALSE;
	TickType_t t1, t2;
	uint32_t load, cycles;
	bool pending;

	/*
	 * Task context with interrupts enabled: the SysTick handler may run
	 * anywhere in this sequence; it changes the tick count, so retry.
	 * Where the handler cannot run (ISR, PendSV, critical section) the
	 * tick count is stable and the loop runs once.
	 */
	do {
		t1 = in_isr ? xTaskGetTickCountFromISR() : xTaskGetTickCount();
		cycles = ove_cortexm_systick_elapsed(&load, &pending);
		t2 = in_isr ? xTaskGetTickCountFromISR() : xTaskGetTickCount();
	} while (t1 != t2);

	*ticks = pending ? t1 + 1 : t1;
	*elapsed = cycles;
}

/** Largest backward step ove_freertos_monotonic() holds, in ticks. */
#define OVE_FREERTOS_MONOTONIC_HOLD_TICKS 2u

/**
 * @brief Clamp @p now so a clock does not step back by less than @p max_hold.
 *
 * The snapshot above is not quite monotonic. While the scheduler is
 * suspended a tick only bumps xPendedTicks: the tick count stands still
 * although VAL keeps restarting, so readings stay inside the stale tick
 * and the one after it (PENDSTSET adds at most one) until xTaskResumeAll
 * catches up - a step back of less than two ticks however long the
 * suspension. Emulated SysTick adds its own error: QEMU computes VAL from
 * host time but raises PENDSTSET from a host timer callback that can run
 * late, so VAL may show a reload before the pending bit does, and the
 * late reload shifts the counter phase by a few microseconds. Such a
 * short step back is held (the clock briefly stands still). A longer one
 * can only be the 32-bit tick count wrapping and is passed through:
 * holding it would freeze the clock for a whole wrap period.
 *
 * The caller must hold the kernel interrupt mask
 * (portSET_INTERRUPT_MASK_FROM_ISR) from before the snapshot until this
 * returns. A sample taken before the mask could be arbitrarily old by
 * the time it is compared - a preempted task - and would pass as a wrap.
 *
 * @param[in,out] last      Per-clock state: the last value returned.
 * @param[in]     now       Clock value computed from a snapshot taken
 *                          under the same mask.
 * @param[in]     max_hold  Longest step back to hold, in the clock's unit
 *                          (OVE_FREERTOS_MONOTONIC_HOLD_TICKS ticks).
 * @return @p now, or the previous value of @p last for a short step back.
 */
static inline uint64_t ove_freertos_monotonic(uint64_t *last, uint64_t now, uint64_t max_hold)
{
	if (now < *last && *last - now <= max_hold)
		return *last;
	*last = now;
	return now;
}

#endif /* OVE_FREERTOS_SYSTICK_H */
