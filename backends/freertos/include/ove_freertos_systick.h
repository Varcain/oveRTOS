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

/**
 * @brief Clamp @p now so a clock never returns less than it did before.
 *
 * On hardware the snapshot above is monotonic. Emulated SysTick is not
 * quite: QEMU computes VAL from host time but raises PENDSTSET from a host
 * timer callback that can run late, so VAL may show a reload before the
 * pending bit does, and the late reload shifts the counter phase by a few
 * microseconds. Clamping keeps such a clock monotonic (it briefly stands
 * still instead of stepping back). The read-compare-store runs under the
 * kernel interrupt mask, so it is safe from task, ISR and PendSV context.
 *
 * @param[in,out] last  Per-clock state: the largest value returned so far.
 * @param[in]     now   Freshly computed clock value.
 * @return max(@p now, previous value of @p last).
 */
static inline uint64_t ove_freertos_monotonic(uint64_t *last, uint64_t now)
{
	UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
	if (now < *last)
		now = *last;
	else
		*last = now;
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
	return now;
}

#endif /* OVE_FREERTOS_SYSTICK_H */
