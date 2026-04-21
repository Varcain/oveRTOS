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
 *   [uint8_t  word_size]      sizeof(uintptr_t) on host (4 or 8)
 *   [uint16_t count]          number of samples
 *   [uint32_t dropped]        samples dropped since last emit
 *   Samples follow, each:
 *     [uint64_t ts_us]
 *     [uint32_t tid]
 *     [uint8_t  depth]        count of valid PCs in the chain
 *     [uint8_t  _pad[3]]
 *     [uint64_t pcs[depth]]   inner-most first (depth entries only)
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
#define OVE_SIM_PROFILER_EVT_SAMPLES   20

/** Event type: profiler capabilities (max Hz, current Hz). */
#define OVE_SIM_PROFILER_EVT_CAPS      21

/** Command type: set sampling rate. Payload = uint32_t Hz (LE). */
#define OVE_SIM_PROFILER_CMD_SET_RATE  100

/** Envelope version. */
#define OVE_SIM_PROFILER_VERSION       1

/** Drain period for samples (ms). */
#define OVE_SIM_PROFILER_DRAIN_MS      200

/*
 * Upper bound on samples per emission. Each sample is 16 B header +
 * depth * 8 B. Budget is the bridge's 4 KiB cap minus the envelope,
 * leaving ~4080 B for samples. With depth=16 that's 144 B/sample, so
 * cap at 24 samples/batch to stay safely under the limit.
 */
#define OVE_SIM_PROFILER_MAX_BATCH     24

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
