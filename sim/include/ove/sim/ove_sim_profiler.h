/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_profiler Profiler Simulation Plugin
 * @brief Drains the profiler sample ring and forwards samples to the dashboard.
 *
 * Emits a single event type (OVE_SIM_PROFILER_EVT_SAMPLES) carrying one
 * or more samples. Envelope:
 *
 *   [uint8_t  version]        1
 *   [uint8_t  word_size]      bytes per PC: sizeof(uintptr_t) (4 on Cortex-M)
 *   [uint16_t count]          number of samples
 *   [uint32_t dropped]        samples dropped since last emit
 *   Samples follow, each:
 *     [uint64_t ts_us]
 *     [uint32_t tid]
 *     [uint8_t  depth]        count of valid PCs in the chain
 *     [uint8_t  state]        ove_thread_state_t at sample time
 *     [uint16_t isr]          interrupted handler's exception number,
 *                             0 for thread-mode samples
 *     [word     pcs[depth]]   word_size bytes each, inner-most first
 *
 * A batch holds as many full-depth samples as fit in
 * OVE_SIM_EVENT_MAX_BYTES, derived from CONFIG_OVE_PROFILER_MAX_DEPTH.
 *
 * The dashboard receives these as FRAME_PROFILE (0x0F) and aggregates
 * them into a flat table (by leaf PC) and a flame graph (by chain).
 * Symbols come from the host: the bridge parses the firmware ELF with
 * the cross nm and sends them to the dashboard as a FRAME_PROFILE
 * symbols sub-frame.
 * @{
 */

#ifndef OVE_SIM_PROFILER_H
#define OVE_SIM_PROFILER_H

#include "ove_sim_plugin.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Event type: a batch of samples. */
#define OVE_SIM_PROFILER_EVT_SAMPLES (OVE_SIM_EVT_BASE_PROFILER + 0)

/**
 * Event type: profiler capabilities. Payload (LE):
 *   [uint32_t max_hz]      compile-time sampling ceiling
 *   [uint32_t current_hz]  rate in use
 *   [uint32_t flags]       OVE_PROFILER_FLAG_* (absent in older firmware)
 */
#define OVE_SIM_PROFILER_EVT_CAPS (OVE_SIM_EVT_BASE_PROFILER + 1)

/** Command type: set sampling rate. Payload = uint32_t Hz (LE). */
#define OVE_SIM_PROFILER_CMD_SET_RATE (OVE_SIM_CMD_BASE_PROFILER + 0)

/** Envelope version. */
#define OVE_SIM_PROFILER_VERSION 1

/**
 * Drain period for samples (ms).
 *
 * The consolidated sim-debug pump ticks every 50 ms, and the event
 * budget the drain may spend is a *per pump iteration* budget (see
 * sim/include/ove/sim/ove_sim_budget.h): a drain that runs less often
 * than the pump leaves most of that budget unspent and halves the
 * number of samples the transport can carry per second.  The drain
 * therefore runs on every pump tick, and the sustained drain rate is
 *
 *   PROFILE_MAX_BATCHES_PER_TICK * PROFILE_MAX_BATCH * 1000 / OVE_SIM_PROFILER_DRAIN_MS
 *
 * samples/s (5 600/s at the default depth of 32), against the
 * CONFIG_OVE_PROFILER_HZ (1 000/s) a single-sample-per-period backend
 * produces and the `hz * threads` the NuttX wall-clock backend
 * produces.
 */
#define OVE_SIM_PROFILER_DRAIN_MS 50

/**
 * @brief Register the profiler plugin.
 * @return Non-negative plugin ID on success, 0 when disabled at
 *         compile time, negative ove error code on failure.
 */
int ove_sim_profiler_register(void);

/**
 * @brief Drain the sample ring and emit batches to the dashboard.
 *
 * Called from the consolidated sim-debug pump. No-op when the profiler
 * is compiled out.
 */
void ove_sim_profiler_tick(void);

/**
 * @brief Queue a capabilities event (max_hz, current_hz) for the dashboard.
 *
 * The dashboard uses max_hz to filter its rate dropdown down to
 * achievable values and current_hz to set the initial selection.  The
 * event is sent by the next ove_sim_profiler_tick(), which sends at most
 * one per tick however often it was requested.  Called once after
 * registration; rate changes queue one by themselves.
 */
void ove_sim_profiler_announce_caps(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_PROFILER_H */
