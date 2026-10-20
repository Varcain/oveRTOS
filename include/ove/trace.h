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
 * additionally emits point markers at sync-primitive entry/exit and
 * enables the user instrumentation API: named marks and zones
 * (ove_trace_register_name(), ove_trace_mark(), ove_trace_zone_begin(),
 * ove_trace_zone_end()). The mark and zone calls may also be made from an
 * interrupt handler; their records then land on the interrupt lane.
 *
 * Thread identity is the low 32 bits of the backend thread handle. The
 * dashboard resolves these to names via descriptor records emitted
 * periodically by the sim plugin.
 */

#ifndef OVE_TRACE_H
#define OVE_TRACE_H

#include <stdbool.h>
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
	OVE_TRACE_ACT_ZONE_BEGIN = 5,
	OVE_TRACE_ACT_ZONE_END = 6,
};

/** @brief When ove_trace_emit_kthread_state() emits the transition. */
#define OVE_TRACE_KT_ALWAYS 0	    /**< Whenever the state changes. */
#define OVE_TRACE_KT_FROM_WAITING 1 /**< Only from BLOCKED or SUSPENDED. */
#define OVE_TRACE_KT_FROM_RUNNING 2 /**< Only from RUNNING. */

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
 * @brief Emit a thread state transition with a caller-supplied timestamp.
 *
 * For hot paths that also feed the state tracker: read the clock once
 * (ove_state_stats_now_us()) and pass the same time to both.
 *
 * @param[in] thread_handle Stable backend handle of the thread changing state.
 * @param[in] old_state     Previous state (@c ove_thread_state_t encoding).
 * @param[in] new_state     New state (@c ove_thread_state_t encoding).
 * @param[in] ts_us         Timestamp, microseconds.
 */
void ove_trace_emit_state_at(uintptr_t thread_handle, int old_state, int new_state, uint64_t ts_us);

/**
 * @brief Emit a sync-primitive marker.
 *
 * @param[in] thread_handle Stable backend handle of the emitting thread.
 * @param[in] prim          One of @c OVE_TRACE_PRIM_*.
 * @param[in] act           One of @c OVE_TRACE_ACT_*.
 * @param[in] object        Address of the primitive (its low 32 bits are
 *                          carried in the record).
 */
void ove_trace_emit_mark(uintptr_t thread_handle, uint8_t prim, uint8_t act, uintptr_t object);

#ifdef CONFIG_OVE_TRACE_MARKERS
/**
 * @brief Register a human-readable name for a user trace id.
 *
 * Names are emitted to the dashboard in a separate trace descriptor batch
 * and resolve user marks/zones from an id to a string. The trace layer
 * copies the name, so the caller's buffer does not have to outlive the
 * call — it may be freed or reused as soon as the call returns. The copy
 * is truncated to 31 characters, the limit of the trace wire format.
 *
 * The table holds up to @c OVE_TRACE_NAME_MAX ids; re-registering an id
 * replaces its name.
 *
 * Threading: the table may be read while it is written (the sim pump
 * reads it from its own thread), but the registrations themselves must
 * come from a single thread, and they should happen before the sim pump
 * starts. A late registration is still picked up, but a name that is
 * replaced while the pump reads it may be reported with its previous
 * value until the next refresh.
 *
 * @param[in] id   User id in the range 0..65535.
 * @param[in] name NUL-terminated name; copied by the trace layer.
 *
 * @retval true  The name was registered.
 * @retval false The name was rejected — it was empty, or the name table
 *               was full.
 */
bool ove_trace_register_name(uint16_t id, const char *name);

/**
 * @brief Emit a point marker for a user id.
 *
 * Appears as a marker on the calling thread's lane, shown with the name
 * registered for @p id.
 *
 * @param[in] id User id in the range 0..65535.
 */
void ove_trace_mark(uint16_t id);

/**
 * @brief Open a named user zone (e.g. an expensive function or phase).
 *
 * Pair with @ref ove_trace_zone_end. Zones may nest on the same thread,
 * also with the same id; the dashboard draws each from begin to end.
 *
 * @param[in] id User id in the range 0..65535.
 */
void ove_trace_zone_begin(uint16_t id);

/**
 * @brief Close the most recent zone with @p id on the calling thread.
 *
 * That is the innermost one still open: an end closes zones with the
 * same id in the reverse order of their begins.
 *
 * @param[in] id User id in the range 0..65535.
 */
void ove_trace_zone_end(uint16_t id);

#endif /* CONFIG_OVE_TRACE_MARKERS */

/**
 * @brief Emit a state transition for a thread with no ove_thread wrapper.
 *
 * Idle and kernel worker threads have no embedded tracker; the trace
 * layer keeps their previous state in a small per-tid table so the
 * swimlane can show their lane.  Callable from ISR and context-switch
 * context, but the table is not locked: callers must serialise (call
 * with interrupts masked or from inside the scheduler).  A thread takes
 * a slot only with an emitted transition and frees it on TERMINATED.
 *
 * @param[in] tid        Shared thread identity (trace tid).
 * @param[in] new_state  New state (@c ove_thread_state_t encoding).
 * @param[in] when       @c OVE_TRACE_KT_ALWAYS, or emit only from a waiting
 *                       state (@c OVE_TRACE_KT_FROM_WAITING) or only from
 *                       RUNNING (@c OVE_TRACE_KT_FROM_RUNNING).
 */
void ove_trace_emit_kthread_state(uintptr_t tid, int new_state, int when);

/**
 * @brief Return the current thread's trace id.
 *
 * Supplied by the thread backend. Used by
 * @c OVE_TRACE_MARK_CURRENT to tag markers emitted from sync-primitive
 * code that doesn't otherwise have the thread pointer in scope.
 *
 * @return The calling ove_thread, else the kernel thread handle; 0 in
 *         interrupt context or when there is no current thread.
 */
uintptr_t ove_backend_thread_current_handle(void);

#else

static inline void ove_trace_emit_state(uintptr_t t, int o, int n)
{
	(void)t;
	(void)o;
	(void)n;
}

static inline void ove_trace_emit_state_at(uintptr_t t, int o, int n, uint64_t ts)
{
	(void)t;
	(void)o;
	(void)n;
	(void)ts;
}

static inline void ove_trace_emit_mark(uintptr_t t, uint8_t p, uint8_t a, uintptr_t o)
{
	(void)t;
	(void)p;
	(void)a;
	(void)o;
}

static inline void ove_trace_emit_kthread_state(uintptr_t t, int n, int w)
{
	(void)t;
	(void)n;
	(void)w;
}

static inline uintptr_t ove_backend_thread_current_handle(void)
{
	return 0;
}

#endif /* CONFIG_OVE_TRACE_STREAM */

#if defined(CONFIG_OVE_TRACE_STREAM) && defined(CONFIG_OVE_TRACE_MARKERS)
#define OVE_TRACE_MARK(thread, prim, act, obj) \
	ove_trace_emit_mark((uintptr_t)(thread), (prim), (act), (uintptr_t)(obj))
#define OVE_TRACE_MARK_CURRENT(prim, act, obj) \
	ove_trace_emit_mark(ove_backend_thread_current_handle(), (prim), (act), (uintptr_t)(obj))
#else
#define OVE_TRACE_MARK(thread, prim, act, obj) ((void)0)
#define OVE_TRACE_MARK_CURRENT(prim, act, obj) ((void)0)
static inline bool ove_trace_register_name(uint16_t id, const char *name)
{
	(void)id;
	(void)name;
	return false;
}
static inline void ove_trace_mark(uint16_t id)
{
	(void)id;
}
static inline void ove_trace_zone_begin(uint16_t id)
{
	(void)id;
}
static inline void ove_trace_zone_end(uint16_t id)
{
	(void)id;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* OVE_TRACE_H */
