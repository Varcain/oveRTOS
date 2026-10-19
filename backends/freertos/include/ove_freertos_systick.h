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
 * PendSV and critical-section context.
 *
 * The hazard: SysTick can reload while its handler cannot run — inside a
 * higher- or equal-priority exception (PendSV, where the context-switch
 * trace hooks run) or a critical section (BASEPRI). VAL has then restarted
 * but xTickCount has not been incremented yet, so a naive
 * `ticks * period + (LOAD - VAL)` lands one whole tick in the past. A
 * re-read of the tick count cannot detect this, because the handler is
 * masked. SCB->ICSR.PENDSTSET can: it is set by the reload and cleared
 * when the handler is entered.
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

#define OVE_SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)
#define OVE_SYSTICK_VAL (*(volatile uint32_t *)0xE000E018u)
#define OVE_SCB_ICSR (*(volatile uint32_t *)0xE000ED04u)
#define OVE_SCB_ICSR_PENDSTSET (1u << 26)

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
	const uint32_t load = OVE_SYSTICK_LOAD;
	TickType_t t1, t2;
	uint32_t val, pending;

	/*
	 * Task context with interrupts enabled: the SysTick handler may run
	 * anywhere in this sequence; it changes the tick count, so retry.
	 * Where the handler cannot run (ISR, PendSV, critical section) the
	 * tick count is stable and the loop runs once.
	 *
	 * VAL is read before PENDSTSET: a reload between the two reads leaves
	 * PENDSTSET set, and VAL is then re-read after the reload.
	 */
	do {
		t1 = in_isr ? xTaskGetTickCountFromISR() : xTaskGetTickCount();
		val = OVE_SYSTICK_VAL;
		pending = OVE_SCB_ICSR & OVE_SCB_ICSR_PENDSTSET;
		if (pending)
			val = OVE_SYSTICK_VAL;
		t2 = in_isr ? xTaskGetTickCountFromISR() : xTaskGetTickCount();
	} while (t1 != t2);

	*ticks = pending ? t1 + 1 : t1;
	*elapsed = val <= load ? load - val : 0u;
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
