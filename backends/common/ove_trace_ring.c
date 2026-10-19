/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

#include <stdatomic.h>
#include <string.h>

#include "ove_trace_ring.h"

#ifndef CONFIG_OVE_TRACE_RING_SIZE
#define CONFIG_OVE_TRACE_RING_SIZE 4096
#endif

#define TRACE_RING_SIZE ((unsigned)CONFIG_OVE_TRACE_RING_SIZE)

/*
 * Lock-free multi-producer / single-consumer ring.
 *
 * Producers are thread code, kernel context-switch hooks (FreeRTOS
 * PendSV) and ISRs.  Any of them can preempt another producer at any
 * point, so a lock — even a try-lock — is unusable: a producer
 * preempted while holding it would make every other producer drop
 * until it ran again.
 *
 * Instead, a producer reserves slot `w` by CAS-advancing the write
 * index, fills buf[w % SIZE], then publishes it by release-storing
 * committed[w % SIZE] = 1.  The drainer walks forward from `read`,
 * copies committed slots, clears their commit byte, and stops at the
 * first uncommitted slot — so records are delivered in reservation
 * order even when a preempted producer finishes its store late (the
 * later records wait for the next drain).  Only a genuinely full ring
 * increments `dropped`.
 *
 * Same scheme as backends/common/ove_profiler_ring.c.
 */
static struct {
	struct ove_trace_record buf[TRACE_RING_SIZE];
	atomic_uchar committed[TRACE_RING_SIZE];
	atomic_uint write;
	atomic_uint read;
	atomic_uint dropped;
} ring;

bool ove_trace_ring_push(const struct ove_trace_record *rec)
{
	unsigned w = atomic_load_explicit(&ring.write, memory_order_relaxed);
	for (;;) {
		unsigned r = atomic_load_explicit(&ring.read, memory_order_acquire);
		if ((unsigned)(w - r) >= TRACE_RING_SIZE) {
			atomic_fetch_add_explicit(&ring.dropped, 1u, memory_order_relaxed);
			return false;
		}
		/* On failure `w` is reloaded with the current write index. */
		if (atomic_compare_exchange_weak_explicit(
			    &ring.write, &w, w + 1u, memory_order_acq_rel, memory_order_relaxed))
			break;
	}

	unsigned idx = w % TRACE_RING_SIZE;
	ring.buf[idx] = *rec;
	/* Pairs with the drainer's acquire load of committed[idx]. */
	atomic_store_explicit(&ring.committed[idx], 1u, memory_order_release);
	return true;
}

size_t ove_trace_ring_drain(struct ove_trace_record *out, size_t max)
{
	unsigned r = atomic_load_explicit(&ring.read, memory_order_relaxed);
	unsigned w = atomic_load_explicit(&ring.write, memory_order_acquire);
	size_t copied = 0;

	while (r != w && copied < max) {
		unsigned idx = r % TRACE_RING_SIZE;
		if (!atomic_load_explicit(&ring.committed[idx], memory_order_acquire))
			break; /* reserved but not yet filled — next drain */
		out[copied++] = ring.buf[idx];
		/* Clear before publishing `read`, so a producer that sees the
		 * slot as free (via its acquire load of `read`) also sees it
		 * uncommitted. */
		atomic_store_explicit(&ring.committed[idx], 0u, memory_order_relaxed);
		r++;
	}

	atomic_store_explicit(&ring.read, r, memory_order_release);
	return copied;
}

uint32_t ove_trace_ring_dropped_get(void)
{
	return atomic_exchange_explicit(&ring.dropped, 0u, memory_order_relaxed);
}

#endif /* CONFIG_OVE_TRACE_STREAM */
