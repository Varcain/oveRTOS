/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_trace Trace Simulation Plugin
 * @brief Drains the in-kernel trace ring and forwards records to the dashboard.
 *
 * Emits two event types:
 *
 *  OVE_SIM_TRACE_EVT_STREAM       - batched trace records
 *  OVE_SIM_TRACE_EVT_DESCRIPTORS  - tid -> name mapping (periodic)
 *
 * Frame payload (both subtypes share a common envelope):
 *   [uint8_t  sub_type]          0 = DESCRIPTORS, 1 = STREAM
 *   [uint8_t  version]           1
 *   [uint16_t count]             number of records
 *   [uint32_t dropped]           records dropped since last emit
 *   Records follow, @count of them:
 *     STREAM: 16 bytes each — matches struct ove_trace_record
 *     DESCRIPTORS:
 *       [uint32_t tid]
 *       [uint8_t  name_len]   (capped at 31)
 *       [char     name[name_len]]
 *
 * Rendered by the dashboard as a per-thread swimlane.
 * @{
 */

#ifndef OVE_SIM_TRACE_H
#define OVE_SIM_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Event-type namespace: the sim bridge currently dispatches to dashboard
 * frames on event_type alone (not plugin_id), so each plugin must pick
 * values that don't collide with others. The debug plugin owns 0
 * (SIM_DEBUG_EVT_THREADS). Reserve 10+ for the trace plugin.
 */

/** Event type: a batch of stream records. */
#define OVE_SIM_TRACE_EVT_STREAM 10

/** Event type: the tid->name descriptor table. */
#define OVE_SIM_TRACE_EVT_DESCRIPTORS 11

/** Envelope sub_type byte values. */
#define OVE_SIM_TRACE_SUB_DESCRIPTORS 0
#define OVE_SIM_TRACE_SUB_STREAM 1

/** Envelope version (bumped on incompatible format change). */
#define OVE_SIM_TRACE_VERSION 1

/** Drain period for stream records (ms). */
#define OVE_SIM_TRACE_DRAIN_MS 100

/** Interval between descriptor emissions (ms). */
#define OVE_SIM_TRACE_DESC_MS 500

/** Maximum records per stream emission. Keeps the per-event payload
 *  (envelope 8 + records * 16) below the bridge's 4 KiB message cap. */
#define OVE_SIM_TRACE_MAX_BATCH 128

/** Maximum threads per descriptor emission. */
#define OVE_SIM_TRACE_MAX_DESC 32

/** Maximum descriptor name length included in the emission. */
#define OVE_SIM_TRACE_MAX_NAME 31

/**
 * @brief Register the trace plugin (no-op if CONFIG_OVE_TRACE_STREAM is off).
 * @return Non-negative plugin ID on success, 0 when trace is disabled,
 *         negative ove error code on failure.
 */
int ove_sim_trace_register(void);

/**
 * @brief Drain the trace ring and emit batches to the dashboard.
 *
 * Called from the consolidated sim-debug pump. @elapsed_ms is the time
 * since the previous tick, used to schedule periodic descriptor
 * emissions. No-op when trace is compiled out.
 */
void ove_sim_trace_tick(uint32_t elapsed_ms);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_TRACE_H */
