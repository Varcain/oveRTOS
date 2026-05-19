/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file trace.h
 * @brief Thread-state / sync-primitive trace stream.
 *
 * When CONFIG_OVE_TRACE_STREAM is enabled, every thread state transition
 * is pushed into a lock-free ring and drained by the sim transport, which
 * forwards it to the dashboard as a swimlane. CONFIG_OVE_TRACE_MARKERS
 * additionally emits point markers at sync-primitive entry/exit.
 *
 * Thread identity is the low 32 bits of the backend thread handle. The
 * dashboard resolves these to names via descriptor records emitted
 * periodically by the sim plugin.
 */

#ifndef OVE_TRACE_H
#define OVE_TRACE_H

#include <stdint.h>

#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Record kind (top-level classification). */
enum {
	OVE_TRACE_KIND_STATE = 1,
	OVE_TRACE_KIND_MARK = 2,
};

/* MARK primitive types — high nibble of record.code. */
enum {
	OVE_TRACE_PRIM_MUTEX = 1,
	OVE_TRACE_PRIM_SEM = 2,
	OVE_TRACE_PRIM_EVENT = 3,
	OVE_TRACE_PRIM_CV = 4,
	OVE_TRACE_PRIM_QUEUE = 5,
	OVE_TRACE_PRIM_USER = 15,
};

/* MARK actions — low nibble of record.code. */
enum {
	OVE_TRACE_ACT_WAIT_ENTER = 1,
	OVE_TRACE_ACT_WAIT_EXIT = 2,
	OVE_TRACE_ACT_POST = 3,
	OVE_TRACE_ACT_USER = 4,
};

#ifdef CONFIG_OVE_TRACE_STREAM

/**
 * @brief Emit a thread state transition.
 *
 * Called from backend thread code that already knows the current thread
 * handle.
 *
 * @param[in] thread_handle Stable backend handle of the thread changing state.
 * @param[in] old_state     Previous state, using the @c ove_thread_state_t encoding.
 * @param[in] new_state     New state, using the @c ove_thread_state_t encoding.
 */
void ove_trace_emit_state(uintptr_t thread_handle, int old_state, int new_state);

/**
 * @brief Emit a sync-primitive marker.
 *
 * @param[in] thread_handle Stable backend handle of the emitting thread.
 * @param[in] prim          One of @c OVE_TRACE_PRIM_*.
 * @param[in] act           One of @c OVE_TRACE_ACT_*.
 * @param[in] object        Address of the primitive (low 16 bits survive
 *                          into the record).
 */
void ove_trace_emit_mark(uintptr_t thread_handle, uint8_t prim, uint8_t act, uintptr_t object);

/**
 * @brief Return the current thread's stable handle, or 0 if none.
 *
 * Supplied by the thread backend (posix_thread.c, wasm_thread.c). Used by
 * @c OVE_TRACE_MARK_CURRENT to tag markers emitted from sync-primitive
 * code that doesn't otherwise have the thread pointer in scope.
 *
 * @return Stable backend handle of the calling thread, or 0 if unavailable.
 */
uintptr_t ove_backend_thread_current_handle(void);

#else

static inline void ove_trace_emit_state(uintptr_t t, int o, int n)
{
	(void)t;
	(void)o;
	(void)n;
}

static inline void ove_trace_emit_mark(uintptr_t t, uint8_t p, uint8_t a, uintptr_t o)
{
	(void)t;
	(void)p;
	(void)a;
	(void)o;
}

static inline uintptr_t ove_backend_thread_current_handle(void)
{
	return 0;
}

#endif /* CONFIG_OVE_TRACE_STREAM */

#ifdef CONFIG_OVE_TRACE_MARKERS
#define OVE_TRACE_MARK(thread, prim, act, obj) \
	ove_trace_emit_mark((uintptr_t)(thread), (prim), (act), (uintptr_t)(obj))
#define OVE_TRACE_MARK_CURRENT(prim, act, obj) \
	ove_trace_emit_mark(ove_backend_thread_current_handle(), (prim), (act), (uintptr_t)(obj))
#else
#define OVE_TRACE_MARK(thread, prim, act, obj) ((void)0)
#define OVE_TRACE_MARK_CURRENT(prim, act, obj) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* OVE_TRACE_H */
