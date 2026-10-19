/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host test for the division-free Q32 scaling used by the SysTick time
 * sources (backends/common/ove_cortexm_systick.h): against exact integer
 * division it must never exceed floor(x * num / den), be off by at most
 * one, and be monotonic in x — over whole tick periods at the QEMU
 * (25 MHz) and STM32F746 (216 MHz) core clocks.
 */

#include <stdint.h>
#include <stdio.h>

#include "ove_cortexm_systick.h"

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

/* Cycles -> microseconds within one tick period, as FreeRTOS does
 * (num = 1e6, den = core clock) and NuttX does (num = us per tick,
 * den = cycles per tick). */
static void check_ratio(uint32_t num, uint32_t den, uint32_t limit, uint32_t step)
{
	uint32_t k = ove_q32_factor(num, den);
	uint32_t prev = 0;
	unsigned bad = 0;
	for (uint64_t x = 0; x < limit; x += step) {
		uint32_t got = ove_q32_apply((uint32_t)x, k);
		uint64_t exact = x * num / den;
		if (got > exact || exact - got > 1 || got < prev)
			bad++;
		prev = got;
	}
	/* The last cycle of a period stays below one full tick. */
	uint64_t last = ove_q32_apply(limit - 1u, k);
	CHECK(last < (uint64_t)limit * num / den + 1u);
	if (bad)
		fprintf(stderr, "ratio %u/%u: %u mismatches\n", num, den, bad);
	CHECK(bad == 0);
}

int main(void)
{
	/* FreeRTOS: cycles * 1e6 / SystemCoreClock over one 1 kHz tick
	 * (exhaustive at 25 MHz, every cycle). */
	check_ratio(1000000u, 25000000u, 25000u, 1u);
	check_ratio(1000000u, 216000000u, 216000u, 1u);
	/* A full 24-bit SysTick period (slow tick), sampled. */
	check_ratio(1000000u, 216000000u, 1u << 24, 7u);
	/* NuttX: cycles * USEC_PER_TICK / (LOAD + 1). */
	check_ratio(1000u, 25000u, 25000u, 1u);
	check_ratio(10000u, 2160000u, 2160000u, 3u);

	if (failures) {
		fprintf(stderr, "test_systick_q32: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_systick_q32: OK\n");
	return 0;
}
