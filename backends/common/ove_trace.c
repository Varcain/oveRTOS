/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

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
