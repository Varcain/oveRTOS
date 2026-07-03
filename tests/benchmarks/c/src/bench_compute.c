/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Compute suite — the native side of the "personality tax" comparison
 * (buildroot board/overtos/progs/lbench.c is the FDPIC userspace mirror).
 *
 *  - compute_mix : runs bench_kernel_mix() (bench_kernel.h) — the SAME kernel
 *                  the FDPIC lbench runs, so B1 measures identical work as a
 *                  privileged native thread vs an unprivileged process.  The
 *                  ratio isolates codegen / FDPIC-PIC overhead (should be ≈1).
 *  - null_call   : an out-of-line call + return with no syscall boundary — the
 *                  native floor the personality's raw null_syscall (SVC trap)
 *                  is measured against.  The delta = the syscall-boundary cost.
 */

#include "benchmark.h"
#include "bench_kernel.h"
#include "ove/ove.h"

static volatile uint32_t g_sink; /* defeat dead-code elimination */

/* B1 — identical to lbench's compute_mix. */
static void compute_mix_setup(void *ctx)
{
	(void)ctx;
	uint32_t ck = bench_kernel_mix(1u);
	OVE_LOG_INF("compute kernel checksum: 0x%08x (expect 0x%08x)%s", ck, BENCH_KERNEL_CHECKSUM,
		    ck == BENCH_KERNEL_CHECKSUM ? "" : "  <-- MISMATCH, lbench comparison invalid");
}

static void compute_mix_run(void *ctx)
{
	(void)ctx;
	g_sink ^= bench_kernel_mix(g_sink ? g_sink : 1u);
}

/* B2 floor — plain out-of-line call/return, no kernel boundary.  noinline so
 * the compiler cannot fold it into the caller and measure nothing. */
static uint32_t __attribute__((noinline)) null_call_fn(uint32_t x)
{
	return x + 1u;
}

static void null_call_run(void *ctx)
{
	(void)ctx;
	g_sink = null_call_fn(g_sink);
}

static int compute_is_enabled(void)
{
	return 1;
}

static const bench_case_t compute_cases[] = {
	{
		.name = "compute_mix",
		.type = BENCH_TYPE_LATENCY,
		.setup = compute_mix_setup,
		.run = compute_mix_run,
		.inner_iters = 8, /* ~256-round kernel; amortise the timer read pair */
	},
	{
		.name = "null_call",
		.type = BENCH_TYPE_LATENCY,
		.run = null_call_run,
		.inner_iters = 1000, /* sub-µs call — heavy amortisation */
	},
};

const bench_suite_t bench_suite_compute = {
	.name = "compute",
	.is_enabled = compute_is_enabled,
	.cases = compute_cases,
	.case_count = sizeof(compute_cases) / sizeof(compute_cases[0]),
};
