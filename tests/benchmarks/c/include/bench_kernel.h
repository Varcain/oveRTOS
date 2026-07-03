/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef BENCH_KERNEL_H
#define BENCH_KERNEL_H

#include <stdint.h>

/*
 * Shared deterministic compute kernel for the "personality tax" benchmark
 * (axis B1 — compute).  It is compiled into BOTH worlds:
 *   - the native bench (tests/benchmarks/c, this header), and
 *   - the FDPIC Linux program (buildroot board/overtos/progs/lbench.c, which
 *     carries a byte-identical copy).
 * so B1 measures the SAME work as a privileged native thread and as an
 * unprivileged userspace process — the delta is codegen (native toolchain vs
 * arm-buildroot-uclinuxfdpiceabi-gcc -mfdpic, i.e. FDPIC PIC overhead), which
 * is a real part of the userspace cost we want to see, not a measurement bug.
 *
 * Pure uint32 arithmetic with well-defined unsigned wrap => the result is
 * identical across compiler / -O level / architecture (verified -O0 == -O2).
 * The lbench copy must stay in sync; both self-check against
 * BENCH_KERNEL_CHECKSUM so drift is caught immediately.
 *
 * ~256 xorshift32 + FNV-1a rounds/call (~1 us @216 MHz) — comfortably above
 * the timer-read noise floor so a single call is measurable.
 */
#define BENCH_KERNEL_CHECKSUM 0x855ee3aau /* bench_kernel_mix(1) */

static inline uint32_t bench_kernel_mix(uint32_t seed)
{
	uint32_t x = seed ? seed : 0x2545F491u;
	uint32_t h = 2166136261u; /* FNV-1a offset basis */
	for (int i = 0; i < 256; i++) {
		x ^= x << 13; /* xorshift32 */
		x ^= x >> 17;
		x ^= x << 5;
		h = (h ^ x) * 16777619u; /* FNV-1a prime */
	}
	return h;
}

#endif /* BENCH_KERNEL_H */
