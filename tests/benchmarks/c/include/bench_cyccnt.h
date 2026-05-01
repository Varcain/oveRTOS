/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Bench-only DWT cycle-counter timer (ARMv7-M and ARMv7E-M).
 *
 * Reads the Data Watchpoint and Trace unit's free-running 32-bit cycle
 * counter at 0xE0001004 directly, bypassing the per-RTOS
 * ove_time_get_ns paths (DWT-direct on FreeRTOS, SysTick-stitched on
 * NuttX, k_cycle_get_32 on Zephyr).  Goal: a uniform measurement floor
 * across all three RTOSes on the same hardware.
 *
 * Resolution: 1 cycle (= 1/BENCH_CYCCNT_HZ seconds).
 * Wrap: 32-bit, ~19.86 s at 216 MHz — fine for per-bench-case windows.
 *
 * The functions are header-only inlines so each `bench_cyccnt_read()`
 * compiles to one ARM `LDR` from absolute address — same shape across
 * C / C++ / Rust / Zig once the binding-side wrappers expose the same
 * single-volatile-read primitive (see bench_cyccnt.rs / bench_cyccnt.zig).
 *
 * This timer lives entirely inside the bench tree.  It is *not* part of
 * the oveRTOS public API.
 */

#ifndef BENCH_CYCCNT_H
#define BENCH_CYCCNT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
#define BENCH_CYCCNT_AVAILABLE 1
#else
#define BENCH_CYCCNT_AVAILABLE 0
#endif

/*
 * Override at compile time with -DBENCH_CYCCNT_HZ=<hz> when targeting
 * a board whose CPU is not 216 MHz.  216 MHz matches the
 * STM32F746G-DISCOVERY default across all three RTOS configurations.
 */
#ifndef BENCH_CYCCNT_HZ
#define BENCH_CYCCNT_HZ 216000000u
#endif

#if BENCH_CYCCNT_AVAILABLE

#define BENCH_CYCCNT_DEMCR_ADDR 0xE000EDFCu /* TRCENA at bit 24 */
#define BENCH_CYCCNT_DWT_CTRL_ADDR 0xE0001000u /* CYCCNTENA at bit 0 */
#define BENCH_CYCCNT_DWT_CYCCNT_ADDR 0xE0001004u

static inline void bench_cyccnt_init(void)
{
	*(volatile uint32_t *)BENCH_CYCCNT_DEMCR_ADDR |= (1u << 24);
	*(volatile uint32_t *)BENCH_CYCCNT_DWT_CYCCNT_ADDR = 0u;
	*(volatile uint32_t *)BENCH_CYCCNT_DWT_CTRL_ADDR |= 1u;
}

static inline uint32_t bench_cyccnt_read(void)
{
	return *(volatile uint32_t *)BENCH_CYCCNT_DWT_CYCCNT_ADDR;
}

static inline uint64_t bench_cyccnt_diff_ns(uint32_t start, uint32_t end)
{
	uint32_t cycles = end - start; /* unsigned wrap is well-defined */
	return (uint64_t)cycles * 1000000000ull / (uint64_t)BENCH_CYCCNT_HZ;
}

#else /* !BENCH_CYCCNT_AVAILABLE — POSIX, sim, non-ARM targets */

static inline void bench_cyccnt_init(void)
{
}

#endif /* BENCH_CYCCNT_AVAILABLE */

#ifdef __cplusplus
}
#endif

#endif /* BENCH_CYCCNT_H */
