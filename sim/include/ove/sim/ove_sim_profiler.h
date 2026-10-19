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
 *   [uint8_t  word_size]      bytes per PC on the wire (always 8)
 *   [uint16_t count]          number of samples
 *   [uint32_t dropped]        samples dropped since last emit
 *   Samples follow, each:
 *     [uint64_t ts_us]
 *     [uint32_t tid]
 *     [uint8_t  depth]        count of valid PCs in the chain
 *     [uint8_t  state]        ove_thread_state_t at sample time
 *     [uint8_t  _pad[2]]
 *     [uint64_t pcs[depth]]   inner-most first (depth entries only)
 *
 * A batch holds as many full-depth samples as fit in
 * OVE_SIM_EVENT_MAX_BYTES, derived from CONFIG_OVE_PROFILER_MAX_DEPTH.
 *
 * The dashboard receives these as FRAME_PROFILE (0x0F) and aggregates
 * them into a flat table (by leaf PC) and a flame graph (by chain).
 * Symbols arrive separately from the bridge as a PROFILER_EVT_SYMBOLS
 * frame (built by parsing the ELF with nm -n).
 * @{
 */

#ifndef OVE_SIM_PROFILER_H
#define OVE_SIM_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Event-type namespace — picked to not collide with SIM_DEBUG_EVT_THREADS=0
 * or the trace plugin's 10/11.
 */

/** Event type: a batch of samples. */
#define OVE_SIM_PROFILER_EVT_SAMPLES 20

/**
 * Event type: profiler capabilities. Payload (LE):
 *   [uint32_t max_hz]      compile-time sampling ceiling
 *   [uint32_t current_hz]  rate in use
 *   [uint32_t flags]       OVE_PROFILER_FLAG_* (absent in older firmware)
 */
#define OVE_SIM_PROFILER_EVT_CAPS 21

/**
 * Event type: newly-interned symbols as a JSON payload of the form
 * @c [[pc_start,pc_end,"name"],...] compatible with the dashboard's
 * PROFILE_SUB_SYMBOLS handler. Emitted incrementally by backends that
 * symbolicate on target; none does at present. POSIX never emits this
 * (the bridge forwards a single upfront blob).
 */
#define OVE_SIM_PROFILER_EVT_SYMBOLS 22

/** Command type: set sampling rate. Payload = uint32_t Hz (LE). */
#define OVE_SIM_PROFILER_CMD_SET_RATE 100

/** Envelope version. */
#define OVE_SIM_PROFILER_VERSION 1

/** Drain period for samples (ms). */
#define OVE_SIM_PROFILER_DRAIN_MS 200

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
 * @brief Emit a capabilities event (max_hz, current_hz) to the dashboard.
 *
 * The dashboard uses max_hz to filter its rate dropdown down to
 * achievable values and current_hz to set the initial selection.
 * Called once after registration and after any rate change.
 */
void ove_sim_profiler_announce_caps(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_PROFILER_H */
