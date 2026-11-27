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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fixed-size trace record (wire v3). 24 B keeps the 64-bit timestamp
 * aligned and lets the dashboard treat payloads as a flat array.
 *
 *   ts_us    : CLOCK_MONOTONIC in microseconds
 *   tid      : low 32 bits of the struct ove_thread pointer (stable per
 *              thread, resolved to a name by descriptor records emitted
 *              periodically)
 *   kind     : OVE_TRACE_KIND_STATE or OVE_TRACE_KIND_MARK
 *   code     : STATE -> new_state; MARK -> primitive/action nibbles
 *   reserved : STATE -> old_state; MARK -> unused (kept for v2 layout)
 *   object   : STATE -> old_state; MARK -> object address (low 32 bits) or user id
 *   pad      : always 0; named so that no stack bytes reach the wire
 *
 * Offsets 0..15 match the v2 (16-byte) record, so the dashboard can decode
 * both versions: v2 carries the object's low 16 bits in `reserved`.
 */
struct ove_trace_record {
	uint64_t ts_us;
	uint32_t tid;
	uint8_t kind;
	uint8_t code;
	uint16_t reserved;
	uint32_t object;
	uint32_t pad;
};

#ifdef __cplusplus
#define OVE_TRACE_STATIC_ASSERT static_assert
#else
#define OVE_TRACE_STATIC_ASSERT _Static_assert
#endif

OVE_TRACE_STATIC_ASSERT(sizeof(struct ove_trace_record) == 24, "v3 trace record is 24 bytes");
OVE_TRACE_STATIC_ASSERT(offsetof(struct ove_trace_record, tid) == 8 &&
				offsetof(struct ove_trace_record, kind) == 12 &&
				offsetof(struct ove_trace_record, code) == 13 &&
				offsetof(struct ove_trace_record, reserved) == 14,
			"the first 16 bytes keep the v2 record layout");
OVE_TRACE_STATIC_ASSERT(offsetof(struct ove_trace_record, object) == 16,
			"v3 object word at offset 16");

#undef OVE_TRACE_STATIC_ASSERT

/**
 * ove_trace_ring_push - enqueue a record; drops only when the ring is full
 *
 * Lock-free multi-producer: safe from thread, kernel-hook and ISR context,
 * including when one producer preempts another mid-push. Returns true when
 * committed, false when dropped (and increments the dropped counter).
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
	uint32_t generation; /* new at every ove thread start, 0 for kernel threads */
	const char *name;
};

#ifdef CONFIG_OVE_TRACE_MARKERS
#define OVE_TRACE_NAME_MAX 32
#define OVE_TRACE_NAME_MAX_LEN 31

struct ove_trace_name_desc {
	uint16_t id;
	char name[OVE_TRACE_NAME_MAX_LEN + 1];
};

/* Copy up to max published names in registration order. */
size_t ove_trace_names(struct ove_trace_name_desc *out, size_t max);
#endif

/**
 * ove_backend_trace_list_threads - enumerate live threads as tid + name
 *
 * The backend includes kernel threads so profiler samples of idle and system
 * threads get names. A FreeRTOS task created while the 32-entry registry
 * is full stays absent from the descriptors for its lifetime.
 * tid follows the shared identity rule: the ove_thread for threads created
 * through ove_thread_*, otherwise the kernel's thread handle (0 is
 * reserved for interrupt context). Returns the number of entries written
 * to @out (bounded by @max).
 */
size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_OVE_TRACE_STREAM */

#endif /* OVE_TRACE_RING_H */
