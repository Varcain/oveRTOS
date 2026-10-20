/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

#include <string.h>

#include "ove/thread.h"
#include "ove/trace.h"
#include "ove/thread_state_stats.h"
#include "ove_trace_ring.h"

void ove_trace_emit_state(uintptr_t thread_handle, int old_state, int new_state)
{
	ove_trace_emit_state_at(thread_handle, old_state, new_state, ove_state_stats_now_us());
}

void ove_trace_emit_state_at(uintptr_t thread_handle, int old_state, int new_state, uint64_t ts_us)
{
	struct ove_trace_record rec = {
		.ts_us = ts_us,
		.tid = (uint32_t)thread_handle,
		.kind = OVE_TRACE_KIND_STATE,
		.code = (uint8_t)new_state,
		.reserved = (uint16_t)old_state,
		.object = (uint32_t)old_state,
	};
	(void)ove_trace_ring_push(&rec);
}

/* ── Kernel-thread state tracker ──────────────────────────────────── */

/* Threads without an ove_thread (idle, kernel workers) still deserve a
 * swimlane. They have no ove_state_tracker, so keep the last state in a
 * small table keyed by the shared tid. An ove thread passes through here
 * too, before its own tracker exists, so a slot is taken only by a
 * transition that is emitted, and an exited thread frees its slot. The
 * callers serialise: every backend calls in with interrupts masked or
 * from inside the scheduler. */
#define OVE_TRACE_KTHREAD_MAX 8

static struct {
	uintptr_t tid;
	uint8_t state;
} kthreads[OVE_TRACE_KTHREAD_MAX];

void ove_trace_emit_kthread_state(uintptr_t tid, int new_state, int when)
{
	if (tid == 0)
		return;

	int idx = -1;
	int free_idx = -1;
	for (int i = 0; i < OVE_TRACE_KTHREAD_MAX; i++) {
		if (kthreads[i].tid == tid) {
			idx = i;
			break;
		}
		if (free_idx < 0 && kthreads[i].tid == 0)
			free_idx = i;
	}

	/* A thread seen for the first time is assumed to have been running.
	 * Claim a slot only for a transition that is emitted: the no-op first
	 * observations (switch-in, wake-up of a non-waiting thread) are what
	 * an ove thread produces before its tracker takes over. */
	int prev = idx < 0 ? OVE_THREAD_STATE_RUNNING
			   : __atomic_load_n(&kthreads[idx].state, __ATOMIC_RELAXED);
	bool waiting = prev == OVE_THREAD_STATE_BLOCKED || prev == OVE_THREAD_STATE_SUSPENDED;
	if (prev == new_state || (when == OVE_TRACE_KT_FROM_WAITING && !waiting) ||
	    (when == OVE_TRACE_KT_FROM_RUNNING && prev != OVE_THREAD_STATE_RUNNING))
		return;
	if (idx < 0) {
		if (free_idx < 0)
			return;
		idx = free_idx;
		kthreads[idx].tid = tid;
	}

	uint64_t now = ove_state_stats_now_us();
	ove_trace_emit_state_at(tid, prev, new_state, now);
	if (new_state == OVE_THREAD_STATE_TERMINATED)
		kthreads[idx].tid = 0; /* the address may host a new thread */
	else
		__atomic_store_n(&kthreads[idx].state, (uint8_t)new_state, __ATOMIC_RELAXED);
}

#ifdef CONFIG_OVE_TRACE_MARKERS
/* ── User instrumentation names ───────────────────────────────────── */

/*
 * The name table is written by the application thread
 * (ove_trace_register_name) and read by the sim pump thread
 * (ove_trace_names).  The two run concurrently and share no lock, so
 * the table is published with a sequence lock:
 *
 *   writer: seq -> odd, update the entry, seq -> even
 *   reader: sample seq, copy the entries, sample seq again; when the
 *           counter moved, the copy raced with an update and is
 *           retried.
 *
 * The reader therefore never returns a half-written entry.  The entry
 * count is published separately, with a release store (and read with an
 * acquire load), so an entry is fully initialised before the reader can
 * see it, and the writer only ever fills the slot *above* the published
 * count - the reader never touches a slot that is being filled.
 *
 * Re-registering an id rewrites an already published entry, which the
 * count alone cannot order.  The payload is therefore accessed with
 * relaxed atomics: a concurrent re-registration may make the reader see
 * the old or the new name, but it is never a data race.
 *
 * The table owns the strings: every entry keeps its own copy of the
 * name, truncated to the limit of the trace wire format.  The caller may
 * free or reuse its buffer as soon as the registration returns, and the
 * reader copies the name out without touching the caller's memory.
 */
static struct ove_trace_name_desc trace_names[OVE_TRACE_NAME_MAX];
static size_t trace_name_count; /* published: release / acquire */
static unsigned trace_name_seq; /* sequence lock: even = stable, odd = update in flight */

/* How often the reader retries before it gives up and reports no names. */
#define OVE_TRACE_NAME_SNAPSHOT_ATTEMPTS 4

/* Copy a name into a slot.  The payload is written with relaxed
 * atomics, so a reader copying the same slot concurrently sees either
 * the old or the new name, but never a data race. */
static void trace_name_store(struct ove_trace_name_desc *slot, uint16_t id, const char *name)
{
	size_t len = strlen(name);
	if (len > OVE_TRACE_NAME_MAX_LEN)
		len = OVE_TRACE_NAME_MAX_LEN;

	for (size_t i = 0; i < len; i++)
		__atomic_store_n(&slot->name[i], name[i], __ATOMIC_RELAXED);
	for (size_t i = len; i <= OVE_TRACE_NAME_MAX_LEN; i++)
		__atomic_store_n(&slot->name[i], '\0', __ATOMIC_RELAXED);
	__atomic_store_n(&slot->id, id, __ATOMIC_RELAXED);
}

/* Copy a slot out.  Mirrors trace_name_store(): the shared payload is
 * read with relaxed atomics, the destination belongs to the caller. */
static void trace_name_load(struct ove_trace_name_desc *out, const struct ove_trace_name_desc *slot)
{
	out->id = __atomic_load_n(&slot->id, __ATOMIC_RELAXED);
	for (size_t i = 0; i <= OVE_TRACE_NAME_MAX_LEN; i++)
		out->name[i] = __atomic_load_n(&slot->name[i], __ATOMIC_RELAXED);
}

size_t ove_trace_names(struct ove_trace_name_desc *out, size_t max)
{
	if (out == NULL)
		return 0;

	for (unsigned attempt = 0; attempt < OVE_TRACE_NAME_SNAPSHOT_ATTEMPTS; attempt++) {
		unsigned seq = __atomic_load_n(&trace_name_seq, __ATOMIC_ACQUIRE);
		if (seq & 1u)
			continue; /* an update is in flight - retry */

		size_t n = __atomic_load_n(&trace_name_count, __ATOMIC_ACQUIRE);
		if (n > max)
			n = max;
		for (size_t i = 0; i < n; i++)
			trace_name_load(&out[i], &trace_names[i]);

		/* The copies must not be reordered past the sequence
		 * re-check below.  A full barrier is used instead of
		 * __atomic_thread_fence(), which ThreadSanitizer does not
		 * instrument. */
		__sync_synchronize();
		if (__atomic_load_n(&trace_name_seq, __ATOMIC_ACQUIRE) == seq)
			return n;
	}

	/* The table is rewritten continuously: report no names rather than
	 * a snapshot that may be torn.  The caller keeps the names it had
	 * until the next refresh. */
	return 0;
}

bool ove_trace_register_name(uint16_t id, const char *name)
{
	if (name == NULL || name[0] == '\0')
		return false;

	/* Sequence lock: mark the table as "being updated" so that a
	 * concurrent reader retries instead of copying a half-written
	 * entry. */
	__atomic_add_fetch(&trace_name_seq, 1, __ATOMIC_ACQ_REL);
	/* The entry stores below must not become visible before the odd
	 * sequence: the release half of the increment only orders what came
	 * before it.  A full barrier again, for ThreadSanitizer. */
	__sync_synchronize();

	size_t count = __atomic_load_n(&trace_name_count, __ATOMIC_RELAXED);
	size_t slot = count;
	for (size_t i = 0; i < count; i++) {
		if (__atomic_load_n(&trace_names[i].id, __ATOMIC_RELAXED) == id) {
			slot = i;
			break;
		}
	}

	bool ok = false;
	if (slot < count || count < OVE_TRACE_NAME_MAX) {
		trace_name_store(&trace_names[slot], id, name);
		if (slot == count)
			/* Publish the new entry. */
			__atomic_store_n(&trace_name_count, count + 1, __ATOMIC_RELEASE);
		ok = true;
	}

	/* The table is stable again. */
	__atomic_add_fetch(&trace_name_seq, 1, __ATOMIC_RELEASE);
	return ok;
}

void ove_trace_mark(uint16_t id)
{
	ove_trace_emit_mark(ove_backend_thread_current_handle(), OVE_TRACE_PRIM_USER,
			    OVE_TRACE_ACT_USER, id);
}

void ove_trace_zone_begin(uint16_t id)
{
	ove_trace_emit_mark(ove_backend_thread_current_handle(), OVE_TRACE_PRIM_USER,
			    OVE_TRACE_ACT_ZONE_BEGIN, id);
}

void ove_trace_zone_end(uint16_t id)
{
	ove_trace_emit_mark(ove_backend_thread_current_handle(), OVE_TRACE_PRIM_USER,
			    OVE_TRACE_ACT_ZONE_END, id);
}
#endif /* CONFIG_OVE_TRACE_MARKERS */

void ove_trace_emit_mark(uintptr_t thread_handle, uint8_t prim, uint8_t act, uintptr_t object)
{
	struct ove_trace_record rec = {
		.ts_us = ove_state_stats_now_us(),
		.tid = (uint32_t)thread_handle,
		.kind = OVE_TRACE_KIND_MARK,
		.code = (uint8_t)((prim << 4) | (act & 0x0F)),
		.object = (uint32_t)object,
	};
	(void)ove_trace_ring_push(&rec);
}

#endif /* CONFIG_OVE_TRACE_STREAM */
