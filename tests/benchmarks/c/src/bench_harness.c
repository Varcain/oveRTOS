/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "bench_cyccnt.h"
#include "ove/ove.h"
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING) && defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Direct register access for cache / accelerator disable so the bench
 * harness compiles identically against FreeRTOS, NuttX, and Zephyr —
 * the FreeRTOS port stages stm32f7xx.h on its include path but NuttX
 * and Zephyr resolve STM32F7 registers through different headers we
 * don't want to thread through the bench app.  The addresses and bit
 * positions below are ARMv7-M / STM32F7 spec — they don't change.
 *
 * Cortex-M7 SCB (System Control Block) at 0xE000ED00:
 *   CCR     = +0x14  (Configuration and Control Register)
 *     bit 16: DC   — Data cache enable
 *     bit 17: IC   — Instruction cache enable
 *     bit 18: BP   — Branch prediction enable
 *   CCSIDR  = +0x80  (Cache Size ID Register, read-only)
 *   CSSELR  = +0x84  (Cache Size Selection Register)
 *   ICIALLU = +0x250 (I-Cache Invalidate All to PoU)
 *   DCCISW  = +0x274 (D-Cache Clean and Invalidate by Set/Way)
 *
 * STM32F7 FLASH controller at 0x40023C00:
 *   ACR     = +0x00 (Access Control Register)
 *     bit 8: PRFTEN — flash prefetch enable
 *     bit 9: ARTEN  — ART accelerator enable */
/* clang-format off */
#define _OVE_BENCH_REG32(addr) (*(volatile uint32_t *)(addr))
#define _OVE_BENCH_SCB_BASE     0xE000ED00UL
#define _OVE_BENCH_SCB_CCR      _OVE_BENCH_REG32(_OVE_BENCH_SCB_BASE + 0x014)
#define _OVE_BENCH_SCB_CCSIDR   _OVE_BENCH_REG32(_OVE_BENCH_SCB_BASE + 0x080)
#define _OVE_BENCH_SCB_CSSELR   _OVE_BENCH_REG32(_OVE_BENCH_SCB_BASE + 0x084)
#define _OVE_BENCH_SCB_ICIALLU  _OVE_BENCH_REG32(_OVE_BENCH_SCB_BASE + 0x250)
#define _OVE_BENCH_SCB_DCCISW   _OVE_BENCH_REG32(_OVE_BENCH_SCB_BASE + 0x274)
#define _OVE_BENCH_CCR_DC_Msk  (1UL << 16)
#define _OVE_BENCH_CCR_IC_Msk  (1UL << 17)
#define _OVE_BENCH_CCR_BP_Msk  (1UL << 18)

#define _OVE_BENCH_FLASH_ACR        _OVE_BENCH_REG32(0x40023C00UL)
#define _OVE_BENCH_FLASH_PRFTEN_Msk (1UL << 8)
#define _OVE_BENCH_FLASH_ARTEN_Msk  (1UL << 9)

#define _OVE_BENCH_DSB() __asm volatile ("dsb 0xF" ::: "memory")
#define _OVE_BENCH_ISB() __asm volatile ("isb 0xF" ::: "memory")
/* clang-format on */

/* Replicates CMSIS SCB_DisableICache(): DSB/ISB, clear CCR.IC,
 * invalidate I-cache to PoU, DSB/ISB. */
static inline void _ove_bench_disable_icache(void)
{
	_OVE_BENCH_DSB();
	_OVE_BENCH_ISB();
	_OVE_BENCH_SCB_CCR &= ~_OVE_BENCH_CCR_IC_Msk;
	_OVE_BENCH_SCB_ICIALLU = 0UL;
	_OVE_BENCH_DSB();
	_OVE_BENCH_ISB();
}

/* Replicates CMSIS SCB_DisableDCache(): select L1 D-cache, clear CCR.DC,
 * walk every set/way and clean+invalidate, DSB/ISB.  CCSIDR encoding:
 *   bits  0-2  = LineSize  (log2(line bytes / 16))
 *   bits  3-12 = Associativity − 1 (ways)
 *   bits 13-27 = NumSets − 1       (sets) */
static inline void _ove_bench_disable_dcache(void)
{
	uint32_t ccsidr;
	uint32_t sets;
	uint32_t ways;

	_OVE_BENCH_SCB_CSSELR = 0U; /* select L1 D-cache */
	_OVE_BENCH_DSB();

	ccsidr = _OVE_BENCH_SCB_CCSIDR;
	_OVE_BENCH_SCB_CCR &= ~_OVE_BENCH_CCR_DC_Msk;

	sets = (ccsidr >> 13) & 0x7FFFU;
	do {
		ways = (ccsidr >> 3) & 0x3FFU;
		do {
			_OVE_BENCH_SCB_DCCISW = ((sets & 0x1FFU) << 5) | ((ways & 0x3U) << 30);
		} while (ways-- != 0U);
	} while (sets-- != 0U);

	_OVE_BENCH_DSB();
	_OVE_BENCH_ISB();
}
#endif

/*
 * Bench timer: on ARMv7-M targets read the DWT cycle counter directly
 * so the per-bench-case measurement floor is identical across FreeRTOS,
 * NuttX, and Zephyr.  On non-ARM (POSIX, sim) fall back to
 * ove_time_get_ns since DWT does not exist.  The branch is selected at
 * compile time; the hot path is one inline volatile load.
 */
#if BENCH_CYCCNT_AVAILABLE
static inline uint64_t bench_timestamp(void)
{
	return (uint64_t)bench_cyccnt_read();
}
static inline uint64_t bench_elapsed_ns(uint64_t start, uint64_t end)
{
	return bench_cyccnt_diff_ns((uint32_t)start, (uint32_t)end);
}
#else
static inline uint64_t bench_timestamp(void)
{
	uint64_t t = 0;
	ove_time_get_ns(&t);
	return t;
}
static inline uint64_t bench_elapsed_ns(uint64_t start, uint64_t end)
{
	return end - start;
}
#endif

/* Worst-case timing toggle: at the start of the first bench case,
 * disable every STM32F7 hardware feature that hides flash-fetch
 * latency or otherwise injects non-determinism into per-call timing —
 * Cortex-M7 I-cache + D-cache, branch prediction (SCB->CCR BP bit
 * enabled by stm32f7_mcu_init), and the STM32F7 ART accelerator +
 * flash prefetch buffer (FLASH->ACR).  Approximates the timing of a
 * cacheless ARM MCU (Cortex-M0+, M3, lower-end M4) so the published
 * numbers don't overstate performance for that target class.  See
 * app.yaml for the Kconfig.  Applies to all 4 bindings since this
 * file is shared C. */
static void bench_apply_diagnostics_once(void)
{
#if defined(CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING) && defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	static int worst_case_applied = 0;
	if (!worst_case_applied) {
		_ove_bench_disable_icache();
		_ove_bench_disable_dcache();
		_OVE_BENCH_SCB_CCR &= ~_OVE_BENCH_CCR_BP_Msk;
		_OVE_BENCH_DSB();
		_OVE_BENCH_ISB();
		_OVE_BENCH_FLASH_ACR &= ~(_OVE_BENCH_FLASH_ARTEN_Msk | _OVE_BENCH_FLASH_PRFTEN_Msk);
		_OVE_BENCH_DSB();
		_OVE_BENCH_ISB();
		worst_case_applied = 1;
		OVE_LOG_INF(
			"[diag] worst-case timing: I-cache, D-cache, branch predictor, ART, prefetch DISABLED");
	}
#endif
#if BENCH_CYCCNT_AVAILABLE
	static int cyccnt_inited = 0;
	if (!cyccnt_inited) {
		bench_cyccnt_init();
		cyccnt_inited = 1;
	}
#endif
}

#if CONFIG_OVE_BENCHMARK_PERCENTILES || CONFIG_OVE_BENCHMARK_NOISE_AUDIT
/*
 * Welford running variance — stable and avoids the catastrophic
 * cancellation a naive sum-of-squares would suffer at ns latencies
 * with µs-scale outliers.  Compiled whenever percentiles or the
 * iteration-count noise audit need running mean/stddev.
 */
struct welford {
	uint64_t n;
	double mean; /* ns */
	double m2;
};

static void welford_push(struct welford *w, uint64_t sample)
{
	w->n++;
	double delta = (double)sample - w->mean;
	w->mean += delta / (double)w->n;
	double delta2 = (double)sample - w->mean;
	w->m2 += delta * delta2;
}

static uint64_t welford_stddev_q1000(const struct welford *w)
{
	if (w->n < 2)
		return 0;
	double var = w->m2 / (double)(w->n - 1);
	double sd = 0.0;
	if (var > 0.0) {
		/* Newton-Raphson sqrt — picolibc/newlib's sqrt() pulls in
		 * fenv on bare-metal targets and is overkill for a stddev
		 * report. ~6 iterations converges to sub-ppm accuracy from
		 * any positive seed.
		 */
		double x = var;
		for (int i = 0; i < 8; i++)
			x = 0.5 * (x + var / x);
		sd = x;
	}
	if (sd < 0.0)
		sd = 0.0;
	return (uint64_t)(sd * 1000.0 + 0.5);
}
#endif /* PERCENTILES || NOISE_AUDIT */

#if CONFIG_OVE_BENCHMARK_PERCENTILES
/*
 * Static sample buffer (BSS) — sized at CONFIG_OVE_BENCHMARK_ITERATIONS.
 * Cases that override `iterations` with a higher value get full
 * mean/min/max coverage but their percentiles are computed on the first
 * SAMPLE_BUFFER_SIZE samples only.  Default 1000 samples = 8 KiB.
 */
#define SAMPLE_BUFFER_SIZE CONFIG_OVE_BENCHMARK_ITERATIONS
static uint64_t sample_buffer[SAMPLE_BUFFER_SIZE];

static int u64_cmp(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	return (va > vb) - (va < vb);
}

static void compute_percentiles(uint64_t *samples, unsigned int n, bench_result_t *r)
{
	qsort(samples, n, sizeof(uint64_t), u64_cmp);

	r->p50_ns = samples[(n * 50) / 100];
	r->p95_ns = samples[(n * 95) / 100];
	if (n > 1)
		r->p99_ns = samples[((uint64_t)(n - 1) * 99) / 100];
	else
		r->p99_ns = samples[0];

	/* Trimmed mean: drop top 1% (rounded up to ≥1 sample on small
	 * counts) — robust against the occasional preempted iteration
	 * that would otherwise drag the arithmetic mean upward. */
	unsigned int trim = n / 100;
	if (trim == 0 && n > 10)
		trim = 1;
	unsigned int kept = n - trim;
	if (kept == 0)
		kept = n;
	uint64_t sum = 0;
	for (unsigned int i = 0; i < kept; i++)
		sum += samples[i];
	r->trimmed_mean_ns = sum / kept;
}
#endif /* CONFIG_OVE_BENCHMARK_PERCENTILES */

#if CONFIG_OVE_BENCHMARK_NOISE_AUDIT
/* Iteration-count checkpoints at which to snapshot running mean and
 * stddev for the calibrate-then-lock methodology.  Reader script
 * (scripts/bench_audit.py) plots CV = stddev/mean against N and picks
 * the elbow as the production iteration count. */
static const uint32_t AUDIT_CHECKPOINTS[] = {100, 500, 1000, 2500, 5000, 10000};
#define AUDIT_CHECKPOINT_COUNT (sizeof(AUDIT_CHECKPOINTS) / sizeof(AUDIT_CHECKPOINTS[0]))
#endif

void bench_run_case(const bench_case_t *bc, bench_result_t *result)
{
	bench_apply_diagnostics_once();

	unsigned int iters = bc->iterations;
	unsigned int warmup = CONFIG_OVE_BENCHMARK_WARMUP;

	if (iters == 0)
		iters = CONFIG_OVE_BENCHMARK_ITERATIONS;

#if CONFIG_OVE_BENCHMARK_NOISE_AUDIT
	/* Audit mode: bump default-case iteration count to the highest
	 * checkpoint (10 000) so we get convergence data per case.  Cases
	 * with explicit overrides (delay_1ms, ctx_switch, …) keep their
	 * override so a 10 000-iter run of a 1 ms case doesn't blow past
	 * 10 s — checkpoints higher than `iters` simply don't fire. */
	if (bc->iterations == 0 && iters < 10000)
		iters = 10000;
#endif

	memset(result, 0, sizeof(*result));
	result->min_ns = UINT64_MAX;
	result->heap_delta = -1;

	if (bc->type == BENCH_TYPE_MEMORY) {
		int32_t best = -1;
		int attempts = 3;

		if (bc->setup)
			bc->setup(NULL);

		for (int i = 0; i < attempts; i++) {
			int32_t before = bench_get_free_heap();
			bc->run(NULL);
			int32_t after = bench_get_free_heap();

			if (bc->teardown)
				bc->teardown(NULL);

			if (before >= 0 && after >= 0) {
				int32_t delta = before - after;
				if (delta >= 0 && (best < 0 || delta < best))
					best = delta;
			}
		}

		result->heap_delta = best;
		result->count = 1;
		return;
	}

	if (bc->setup)
		bc->setup(NULL);

	unsigned int inner = bc->inner_iters ? bc->inner_iters : 1;

	/* Warmup */
	for (unsigned int i = 0; i < warmup; i++) {
		for (unsigned int j = 0; j < inner; j++)
			bc->run(NULL);
	}

#if CONFIG_OVE_BENCHMARK_PERCENTILES || CONFIG_OVE_BENCHMARK_NOISE_AUDIT
	struct welford w = {0};
#endif
#if CONFIG_OVE_BENCHMARK_PERCENTILES
	unsigned int sample_count = 0;
#endif

	/* Measurement.  For inner > 1, run() is called inner times per
	 * timestamp pair and elapsed is divided by inner — amortises the
	 * fixed timestamp overhead across multiple operations on sub-µs
	 * benchmarks (e.g. time_get_us_overhead).  The DWT cycle-counter
	 * read used on ARMv7-M is a single LDR (~5 ns at 216 MHz), much
	 * cheaper than the per-RTOS ove_time_get_ns paths. */
	for (unsigned int i = 0; i < iters; i++) {
		uint64_t start = bench_timestamp();
		for (unsigned int j = 0; j < inner; j++)
			bc->run(NULL);
		uint64_t end = bench_timestamp();

		uint64_t elapsed = bench_elapsed_ns(start, end) / inner;

		if (elapsed < result->min_ns)
			result->min_ns = elapsed;
		if (elapsed > result->max_ns)
			result->max_ns = elapsed;
		result->total_ns += elapsed;
		result->count++;

#if CONFIG_OVE_BENCHMARK_PERCENTILES || CONFIG_OVE_BENCHMARK_NOISE_AUDIT
		welford_push(&w, elapsed);
#endif
#if CONFIG_OVE_BENCHMARK_PERCENTILES
		if (sample_count < SAMPLE_BUFFER_SIZE)
			sample_buffer[sample_count++] = elapsed;
#endif
#if CONFIG_OVE_BENCHMARK_NOISE_AUDIT
		for (unsigned int k = 0; k < AUDIT_CHECKPOINT_COUNT; k++) {
			if ((uint32_t)w.n != AUDIT_CHECKPOINTS[k])
				continue;
			if (result->audit_count < BENCH_AUDIT_MAX) {
				bench_audit_point_t *p =
					&result->audit_points[result->audit_count++];
				p->n = (uint32_t)w.n;
				p->mean_ns = (uint64_t)(w.mean + 0.5);
				p->stddev_ns_q = welford_stddev_q1000(&w);
			}
			break;
		}
#endif
	}

	if (bc->teardown)
		bc->teardown(NULL);

	if (bc->type == BENCH_TYPE_THROUGHPUT && result->total_ns > 0) {
		result->ops_per_sec =
			(uint32_t)((uint64_t)result->count * 1000000000ULL / result->total_ns);
	} else if (bc->type == BENCH_TYPE_LATENCY && result->total_ns > 0) {
		result->ops_per_sec =
			(uint32_t)((uint64_t)result->count * 1000000000ULL / result->total_ns);
	}

#if CONFIG_OVE_BENCHMARK_PERCENTILES || CONFIG_OVE_BENCHMARK_NOISE_AUDIT
	if (w.n > 0)
		result->stddev_ns_q = welford_stddev_q1000(&w);
#endif
#if CONFIG_OVE_BENCHMARK_PERCENTILES
	if (sample_count > 0)
		compute_percentiles(sample_buffer, sample_count, result);
#endif
}
