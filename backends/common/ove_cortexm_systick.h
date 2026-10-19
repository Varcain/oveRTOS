/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Sub-tick refinement from the Cortex-M SysTick down-counter, shared by the
 * RTOS time sources that count kernel ticks on SysTick (FreeRTOS, NuttX).
 *
 * The hazard: SysTick can reload while its handler cannot run — inside a
 * higher- or equal-priority exception (PendSV, where context-switch hooks
 * run) or a critical section. VAL has then restarted but the kernel tick
 * count has not been incremented yet, so a naive
 * `ticks * period + (LOAD - VAL)` lands one whole tick in the past. A
 * re-read of the tick count cannot detect this, because the handler is
 * masked. SCB->ICSR.PENDSTSET can: it is set by the reload and cleared
 * when the handler is entered. Callers add one tick when it is set.
 *
 * Callers own the tick count: read it so that it cannot change across
 * ove_cortexm_systick_elapsed() (interrupts masked, or a retry loop that
 * re-reads it afterwards).
 */

#ifndef OVE_CORTEXM_SYSTICK_H
#define OVE_CORTEXM_SYSTICK_H

#include <stdbool.h>
#include <stdint.h>

#define OVE_SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define OVE_SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define OVE_SYST_CVR (*(volatile uint32_t *)0xE000E018u)
#define OVE_SCB_ICSR (*(volatile uint32_t *)0xE000ED04u)
#define OVE_SYST_CSR_ENABLE (1u << 0)
#define OVE_SCB_ICSR_PENDSTSET (1u << 26)

/** True when SysTick is counting (enabled, non-zero reload). */
/* Reading CSR clears its COUNTFLAG: do not use this where something else
 * (a tickless idle path) relies on that flag. */
static inline bool ove_cortexm_systick_running(void)
{
	return (OVE_SYST_CSR & OVE_SYST_CSR_ENABLE) != 0 && OVE_SYST_RVR != 0;
}

/**
 * @brief SysTick cycles elapsed in the current tick period.
 *
 * VAL is read before PENDSTSET: a reload between the two reads leaves
 * PENDSTSET set, and VAL is then re-read after the reload.
 *
 * @param[out] load     Reload value (cycles per tick period minus one).
 * @param[out] pending  True when a reload's tick has not been counted yet:
 *                      the caller must add one tick.
 * @return Cycles since the start of the tick period (0 .. load).
 */
static inline uint32_t ove_cortexm_systick_elapsed(uint32_t *load, bool *pending)
{
	uint32_t rvr = OVE_SYST_RVR;
	uint32_t val = OVE_SYST_CVR;
	bool pend = (OVE_SCB_ICSR & OVE_SCB_ICSR_PENDSTSET) != 0;
	if (pend)
		val = OVE_SYST_CVR;
	*load = rvr;
	*pending = pend;
	return val <= rvr ? rvr - val : 0u;
}

/*
 * Division-free scaling for the hot path (context-switch hooks): a ratio
 * num/den < 1 as a Q32 factor, computed once (one 64-bit division) and
 * applied with a single 32x32->64 multiply. The result is floor-rounded
 * and never exceeds the exact floor(x * num / den), so a sub-tick offset
 * stays below one tick and successive values stay monotonic.
 */

/** floor(num * 2^32 / den); requires 0 < num < den. */
static inline uint32_t ove_q32_factor(uint32_t num, uint32_t den)
{
	return (uint32_t)(((uint64_t)num << 32) / den);
}

/** floor(x * k / 2^32). */
static inline uint32_t ove_q32_apply(uint32_t x, uint32_t k)
{
	return (uint32_t)(((uint64_t)x * k) >> 32);
}

#endif /* OVE_CORTEXM_SYSTICK_H */
