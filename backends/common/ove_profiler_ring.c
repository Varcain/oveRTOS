/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <stdatomic.h>
#include <string.h>

#include "ove_profiler_ring.h"

#ifndef CONFIG_OVE_PROFILER_RING_SIZE
#define CONFIG_OVE_PROFILER_RING_SIZE 1024
#endif

#define PROFILER_RING_SIZE ((unsigned)CONFIG_OVE_PROFILER_RING_SIZE)

/*
 * Lock-free multi-producer / single-consumer ring.
 *
 * Design: producers CAS the write index to reserve a slot, then fill
 * buf[slot] and flip committed[slot] → 1 with release ordering. The
 * drain walks forward from read, copies committed slots, clears the
 * commit byte, and advances read. It stops at the first uncommitted
 * slot so ordering is preserved even when two producers reserve
 * consecutive slots and the earlier one is slow to finish its store.
 *
 * Previous implementation guarded the whole push with an atomic_flag
 * spin-lock and bumped ring.dropped on contention — drops scaled with
 * the number of threads being signalled per tick (N-1 drops per tick
 * in the worst case). The lock-free reservation scheme eliminates
 * that class of drops entirely; only genuine ring-full now increments
 * ring.dropped.
 *
 * Async-signal-safe: only atomic primitives and a plain memcpy into
 * an exclusively-reserved slot — no mutex, no malloc.
 */
static struct {
	struct ove_profiler_sample buf[PROFILER_RING_SIZE];
	atomic_uchar committed[PROFILER_RING_SIZE];
	atomic_uint write;
	atomic_uint read;
	atomic_uint dropped;
} ring;

bool ove_profiler_ring_push(const struct ove_profiler_sample *s)
{
	unsigned w;
	for (;;) {
		w = atomic_load_explicit(&ring.write, memory_order_relaxed);
		unsigned r = atomic_load_explicit(&ring.read, memory_order_acquire);
		if ((unsigned)(w - r) >= PROFILER_RING_SIZE) {
			atomic_fetch_add_explicit(&ring.dropped, 1u, memory_order_relaxed);
			return false;
		}
		/* Reserve slot w. On success we own buf[w % SIZE] until
		 * we publish via committed[]. On failure another producer
		 * got this slot — reload w and retry. */
		if (atomic_compare_exchange_weak_explicit(
			    &ring.write, &w, w + 1u, memory_order_acq_rel, memory_order_relaxed))
			break;
	}

	unsigned idx = w % PROFILER_RING_SIZE;
	ring.buf[idx] = *s;
	/* Release pairs with drain's acquire-load on committed[idx].
	 * Guarantees the consumer sees a fully-initialised sample. */
	atomic_store_explicit(&ring.committed[idx], 1u, memory_order_release);
	return true;
}

size_t ove_profiler_ring_drain(struct ove_profiler_sample *out, size_t max)
{
	unsigned r = atomic_load_explicit(&ring.read, memory_order_relaxed);
	unsigned w = atomic_load_explicit(&ring.write, memory_order_acquire);
	unsigned copied = 0;
	while (r != w && copied < (unsigned)max) {
		unsigned idx = r % PROFILER_RING_SIZE;
		/* Stop at the first uncommitted slot to keep deliveries
		 * in reservation order. A later slot may already be
		 * committed — it just has to wait one more drain tick. */
		if (!atomic_load_explicit(&ring.committed[idx], memory_order_acquire))
			break;
		out[copied++] = ring.buf[idx];
		/* Reset so the next producer wrap to this physical slot
		 * starts from a clean commit state. */
		atomic_store_explicit(&ring.committed[idx], 0u, memory_order_release);
		r++;
	}
	atomic_store_explicit(&ring.read, r, memory_order_release);
	return copied;
}

uint32_t ove_profiler_ring_dropped_get(void)
{
	return atomic_exchange_explicit(&ring.dropped, 0u, memory_order_relaxed);
}

#endif /* CONFIG_OVE_PROFILER */
