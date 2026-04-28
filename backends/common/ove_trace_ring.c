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

#define TRACE_RING_SIZE ((size_t)CONFIG_OVE_TRACE_RING_SIZE)

static struct {
	struct ove_trace_record buf[TRACE_RING_SIZE];
	atomic_uint write;
	atomic_uint read;
	atomic_uint dropped;
	atomic_flag lock;
} ring = {.lock = ATOMIC_FLAG_INIT};

bool ove_trace_ring_push(const struct ove_trace_record *rec)
{
	if (atomic_flag_test_and_set_explicit(&ring.lock, memory_order_acquire)) {
		atomic_fetch_add_explicit(&ring.dropped, 1u, memory_order_relaxed);
		return false;
	}

	unsigned w = atomic_load_explicit(&ring.write, memory_order_relaxed);
	unsigned r = atomic_load_explicit(&ring.read, memory_order_acquire);

	if ((unsigned)(w - r) >= TRACE_RING_SIZE) {
		atomic_flag_clear_explicit(&ring.lock, memory_order_release);
		atomic_fetch_add_explicit(&ring.dropped, 1u, memory_order_relaxed);
		return false;
	}

	ring.buf[w % TRACE_RING_SIZE] = *rec;
	atomic_store_explicit(&ring.write, w + 1u, memory_order_release);
	atomic_flag_clear_explicit(&ring.lock, memory_order_release);
	return true;
}

size_t ove_trace_ring_drain(struct ove_trace_record *out, size_t max)
{
	unsigned w = atomic_load_explicit(&ring.write, memory_order_acquire);
	unsigned r = atomic_load_explicit(&ring.read, memory_order_relaxed);
	unsigned avail = w - r;
	if (avail > max)
		avail = (unsigned)max;

	for (unsigned i = 0; i < avail; ++i)
		out[i] = ring.buf[(r + i) % TRACE_RING_SIZE];

	atomic_store_explicit(&ring.read, r + avail, memory_order_release);
	return avail;
}

uint32_t ove_trace_ring_dropped_get(void)
{
	return atomic_exchange_explicit(&ring.dropped, 0u, memory_order_relaxed);
}

#endif /* CONFIG_OVE_TRACE_STREAM */
