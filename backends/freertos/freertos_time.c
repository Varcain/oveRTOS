/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/time.h"
#include "ove_backend_common.h"
#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

int ove_time_get_us(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	/* HAL_GetTick() is driven by SysTick and gets frozen when
	 * configUSE_TICKLESS_IDLE suppresses ticks across sleep, so any
	 * caller computing now-then deltas across an idle (e.g. ove_pm
	 * time-in-state stats) loses the suspended interval.
	 * xTaskGetTickCount() is the kernel's authoritative tick count and
	 * gets bumped by vTaskStepTick() on wake from tickless sleep, so
	 * deltas remain correct.  xTaskGetTickCountFromISR() is the
	 * ISR-safe variant — same value, callable either way. */
	TickType_t ticks = xPortIsInsideInterrupt() ? xTaskGetTickCountFromISR()
						    : xTaskGetTickCount();
	*out = (uint64_t)ticks * (1000000ULL / configTICK_RATE_HZ);
	return OVE_OK;
}

/* ── 64-bit monotonic nanosecond clock (wrap-stitched, divide-free) ──────────
 * DWT->CYCCNT is 32 bits, so a raw `cycles * 1e9 / clk` wraps every 2^32/clk ≈
 * 19.86 s at 216 MHz — the guest's CLOCK_MONOTONIC jumped BACKWARDS every ~20 s.
 * Maintain a 64-bit nanosecond epoch advanced from the 1 ms tick hook (which
 * samples CYCCNT ~20000× more often than it wraps); the reader adds only the
 * sub-tick delta. Convert with a precomputed Q32 reciprocal instead of a runtime
 * 64-bit software divide (__aeabi_uldivmod). Truncation drift is < 1 ns/tick
 * (< 1 ppm) — negligible next to the oscillator, and the clock stays monotonic. */
static uint64_t g_ns_per_cyc_q32;  /* (1e9 << 32) / SystemCoreClock, computed once */
static volatile uint32_t g_ts_seq; /* seqlock: odd while the tick hook is updating */
static volatile uint64_t g_ts_ns;  /* total ns as of g_ts_cyc */
static volatile uint32_t g_ts_cyc; /* CYCCNT sampled at g_ts_ns */

/* Advance the epoch. Called from vApplicationTickHook (SysTick ISR, 1 ms); the first
 * call arms the reciprocal + epoch. */
void ove_freertos_time_tick(void)
{
	uint32_t now = DWT->CYCCNT;
	if (g_ns_per_cyc_q32 == 0) {
		g_ns_per_cyc_q32 = ((uint64_t)1000000000ULL << 32) / (uint64_t)SystemCoreClock;
		g_ts_cyc = now;
		return;
	}
	uint32_t dcyc = now - g_ts_cyc; /* < 1 ms of cycles, unsigned-wrap-safe */
	uint64_t dns = ((uint64_t)dcyc * g_ns_per_cyc_q32) >> 32;
	g_ts_seq++; /* odd */
	__asm__ volatile("" ::: "memory");
	g_ts_ns += dns;
	g_ts_cyc = now;
	__asm__ volatile("" ::: "memory");
	g_ts_seq++; /* even */
}

int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	uint64_t q = g_ns_per_cyc_q32;
	if (q == 0) { /* before the first tick (only the first ~1 ms of boot): direct read */
		*out = (uint64_t)DWT->CYCCNT * 1000000000ULL / (uint64_t)SystemCoreClock;
		return OVE_OK;
	}
	uint64_t nsbase;
	uint32_t cbase, seq;
	do { /* seqlock: consistent {g_ts_ns, g_ts_cyc} against the tick-hook writer */
		seq = g_ts_seq;
		__asm__ volatile("" ::: "memory");
		nsbase = g_ts_ns;
		cbase = g_ts_cyc;
		__asm__ volatile("" ::: "memory");
	} while ((seq & 1u) || seq != g_ts_seq);
	uint32_t dcyc = DWT->CYCCNT - cbase; /* cycles since the epoch; wrap-safe (< 20 s) */
	*out = nsbase + (((uint64_t)dcyc * q) >> 32);
	return OVE_OK;
}

void ove_time_delay_ms(uint32_t ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

void ove_time_delay_us(uint32_t us)
{
	/* Busy-wait for microsecond delays (no RTOS support) */
	uint32_t start = DWT->CYCCNT;
	uint32_t cycles = (SystemCoreClock / 1000000U) * us;
	while ((DWT->CYCCNT - start) < cycles) {
		/* spin */
	}
}
