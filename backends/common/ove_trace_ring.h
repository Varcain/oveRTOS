/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_TRACE_RING_H
#define OVE_TRACE_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

/*
 * Fixed-size trace record. 16 B keeps alignment trivial and lets the
 * dashboard treat payloads as a flat array.
 *
 *   ts_us : CLOCK_MONOTONIC in microseconds
 *   tid   : low 32 bits of the struct ove_thread pointer (stable per thread,
 *           resolved to a name by descriptor records emitted periodically)
 *   kind  : OVE_TRACE_KIND_STATE or OVE_TRACE_KIND_MARK
 *   code  : STATE -> new_state; MARK -> primitive kind
 *   arg   : STATE -> old_state; MARK -> low 16 bits of object address
 */
struct ove_trace_record {
	uint64_t ts_us;
	uint32_t tid;
	uint8_t  kind;
	uint8_t  code;
	uint16_t arg;
};

/**
 * ove_trace_ring_push - enqueue a record; drops on contention or overflow
 *
 * Safe from any thread context. Returns true when committed, false when
 * dropped (and increments the internal dropped counter).
 */
bool ove_trace_ring_push(const struct ove_trace_record *rec);

/**
 * ove_trace_ring_drain - copy up to @max records into @out, advancing read
 *
 * Consumer-side (single drainer thread — sim_trace plugin). Returns the
 * number of records copied.
 */
size_t ove_trace_ring_drain(struct ove_trace_record *out, size_t max);

/**
 * ove_trace_ring_dropped_get - read and clear the dropped-record counter
 */
uint32_t ove_trace_ring_dropped_get(void);

/*
 * Tid -> name descriptor. Emitted periodically so the dashboard can
 * resolve raw tids in STREAM records to human-readable names.
 */
struct ove_trace_thread_desc {
	uint32_t tid;
	const char *name;
};

/**
 * ove_backend_trace_list_threads - enumerate live thread handles+names
 *
 * Implemented by the thread backend (posix_thread.c, wasm_thread.c).
 * Returns the number of entries written to @out (bounded by @max).
 */
size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out,
				      size_t max);

#endif /* CONFIG_OVE_TRACE_STREAM */

#endif /* OVE_TRACE_RING_H */
