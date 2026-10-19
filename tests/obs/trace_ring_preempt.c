/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * The trace ring, built for test_trace_ring_preempt.c: every atomic load
 * in the ring first calls trace_ring_preempt_point(), where the test runs
 * a nested producer or drainer - the interleaving a preempting ISR,
 * PendSV hook or higher-priority thread produces on a single core.
 */

#include <stdatomic.h>

void trace_ring_preempt_point(void);

/* The builtin stdatomic.h itself expands to: clang only accepts its C11
 * builtin on _Atomic objects, GCC only the __atomic one. */
#undef atomic_load_explicit
#if defined(__clang__)
#define atomic_load_explicit(p, mo) (trace_ring_preempt_point(), __c11_atomic_load((p), (mo)))
#else
#define atomic_load_explicit(p, mo) (trace_ring_preempt_point(), __atomic_load_n((p), (mo)))
#endif

#include "ove_trace_ring.c"
