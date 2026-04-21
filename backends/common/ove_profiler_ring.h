/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_PROFILER_RING_H
#define OVE_PROFILER_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#ifndef CONFIG_OVE_PROFILER_MAX_DEPTH
#define CONFIG_OVE_PROFILER_MAX_DEPTH 16
#endif

/*
 * Fixed-size sample slot. A sample is produced by the backend sampler
 * (usually a signal handler running in the target thread's context).
 *
 *   ts_us : CLOCK_MONOTONIC in microseconds
 *   tid   : low 32 bits of the struct ove_thread pointer
 *   depth : number of valid entries in pcs[]
 *   state : ove_thread_state_t value captured at sample time; lets the
 *           dashboard pivot between wall-clock and on-CPU views
 *   pcs   : inner-most frame first (pcs[0] is where the signal was
 *           delivered); unused slots are undefined.
 *
 * Using fixed-size slots keeps the ring trivial (no variable-length
 * framing) and makes drain-side copying a plain memcpy.
 */
struct ove_profiler_sample {
	uint64_t ts_us;
	uint32_t tid;
	uint8_t  depth;
	uint8_t  state;
	uint8_t  _pad[2];
	uintptr_t pcs[CONFIG_OVE_PROFILER_MAX_DEPTH];
};

/**
 * ove_profiler_ring_push - enqueue a sample; drops only on overflow.
 *
 * Async-signal-safe, lock-free MPSC: producers CAS the write index to
 * reserve a slot, then release-store a per-slot commit byte. Samples
 * are never dropped due to producer contention — only a genuinely full
 * ring increments the dropped counter.
 */
bool ove_profiler_ring_push(const struct ove_profiler_sample *s);

/**
 * ove_profiler_ring_drain - copy up to @max samples to @out, advancing read.
 *
 * Consumer-side — called from the sim_profiler drain thread.
 */
size_t ove_profiler_ring_drain(struct ove_profiler_sample *out, size_t max);

/**
 * ove_profiler_ring_dropped_get - read and clear the dropped counter.
 */
uint32_t ove_profiler_ring_dropped_get(void);

#endif /* CONFIG_OVE_PROFILER */

#endif /* OVE_PROFILER_RING_H */
