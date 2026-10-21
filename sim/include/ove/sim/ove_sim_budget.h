/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_budget Sim Pump Event Budget
 * @brief How the shared-memory event ring is split between the pump's
 *        producers.
 *
 * Every sim plugin reaches the dashboard through the same shared-memory
 * event ring (@c sim/include/ove/sim/ove_sim_shm.h, @c SIM_SHM_RING_SIZE
 * bytes).  The ring does not block: a producer that overruns it has its
 * event DROPPED and @c sim_shm_header::event_dropped is bumped.  A
 * backlog that is dropped is destroyed, not deferred - the failure this
 * budget exists to remove.
 *
 * The consolidated sim-debug pump (@c sim/hal/sim_debug.c) drives the
 * producers that share one iteration, and the ring is a *per iteration*
 * budget: the host is allowed to lag behind by a whole pump iteration,
 * but not by more.  Two drains write to the ring from the pump:
 *
 *   - the profiler sample drain (@c sim/hal/sim_profiler.c),
 *   - the trace stream drain (@c sim/hal/sim_trace.c),
 *
 * and two more producers can land in the same iteration:
 *
 *   - the trace's thread-descriptor and name-table events, emitted by
 *     the trace plugin itself (charged to the trace's share),
 *   - the debug plugin's thread/heap snapshot (@c sim/hal/sim_debug.c),
 *     one event per pump iteration.  Its size is owned by that file, so
 *     the share it takes is reserved here as a whole
 *     @c OVE_SIM_EVENT_MAX_BYTES event - the largest event the host
 *     bridge forwards.
 *
 * The split below is the single place that knows how the ring is
 * divided, so the two drains cannot drift apart and the sum of the
 * shares always fits in the ring.
 *
 * @{
 */

#ifndef OVE_SIM_BUDGET_H
#define OVE_SIM_BUDGET_H

#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes one pump iteration may hand to the event ring. */
#define SIM_EVENT_RING_BUDGET_BYTES SIM_SHM_RING_SIZE

/**
 * Bytes reserved for the debug plugin's thread/heap snapshot.
 *
 * The snapshot is emitted by @c sim/hal/sim_debug.c, which owns its
 * layout; the reservation is therefore a whole maximum-size event plus
 * the ring's two-byte length prefix, rather than a copy of that layout.
 * The bridge skips larger events, so no snapshot can exceed
 * @c OVE_SIM_EVENT_MAX_BYTES.
 */
#define SIM_EVENT_SNAPSHOT_RESERVE_BYTES (2u + OVE_SIM_EVENT_MAX_BYTES)

/** Bytes the two ring drains may claim in one pump iteration. */
#define SIM_EVENT_DRAIN_BUDGET_BYTES \
	(SIM_EVENT_RING_BUDGET_BYTES - SIM_EVENT_SNAPSHOT_RESERVE_BYTES)

/**
 * The profiler drain's share of the drain budget.
 *
 * The profiler gets two thirds of it: its samples are the largest
 * items on the ring (16 + depth * sizeof(uintptr_t) bytes each, 144 B
 * at the default depth of 32) and its ring the smallest
 * (@c CONFIG_OVE_PROFILER_RING_SIZE samples against the trace's
 * @c CONFIG_OVE_TRACE_RING_SIZE records), so it is the producer that
 * breaks first when the budget is too small.
 */
#define SIM_EVENT_PROFILER_BUDGET_BYTES (SIM_EVENT_DRAIN_BUDGET_BYTES * 2u / 3u)

/** The trace drain's share of the drain budget. */
#define SIM_EVENT_TRACE_BUDGET_BYTES \
	(SIM_EVENT_DRAIN_BUDGET_BYTES - SIM_EVENT_PROFILER_BUDGET_BYTES)

/**
 * Bytes one plugin event occupies in the ring: the two-byte length
 * prefix the ring protocol uses plus the event header.
 */
#define SIM_EVENT_RING_OVERHEAD_BYTES (2u + sizeof(struct ove_sim_event))

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_BUDGET_H */
