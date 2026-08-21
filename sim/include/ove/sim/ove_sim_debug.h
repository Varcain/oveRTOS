/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_debug Debug Simulation Plugin
 * @brief Periodic thread and memory statistics for the sim dashboard.
 *
 * Emits snapshots of thread state and heap usage at ~2 Hz via the
 * standard plugin event mechanism.  The dashboard bridge forwards
 * these as FRAME_THREAD (0x0A) WebSocket frames.
 *
 * Binary snapshot format (event payload):
 *   [uint8_t  thread_count]
 *   [uint32_t heap_total]
 *   [uint32_t heap_free]
 *   [uint32_t heap_used]
 *   [uint32_t heap_peak]
 *   Per thread (repeated thread_count times):
 *     [uint8_t  name_len]       (max 16)
 *     [char     name[name_len]]
 *     [uint8_t  state]          (ove_thread_state_t)
 *     [uint8_t  priority]
 *     [uint32_t valid_fields]   (OVE_THREAD_INFO_VALID_* bits)
 *     [uint32_t stack_used]     (high-water mark bytes)
 *     [uint32_t stack_size]     (total allocation bytes)
 *     [uint32_t cpu_x100]       (cpu_percent_x100)
 *     [uint32_t state_x100[4]]  (running, ready, blocked, suspended)
 * @{
 */

#ifndef OVE_SIM_DEBUG_H
#define OVE_SIM_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Event type for thread/memory snapshots. */
#define OVE_SIM_DEBUG_EVT_THREADS 0

/** Maximum threads reported per snapshot. */
#define OVE_SIM_DEBUG_MAX_THREADS 16

/** Maximum thread name length in snapshot. */
#define OVE_SIM_DEBUG_MAX_NAME_LEN 16

/** Snapshot interval in milliseconds (~2 Hz). */
#define OVE_SIM_DEBUG_INTERVAL_MS 500

/**
 * @brief Register the debug simulation plugin.
 * @return Non-negative plugin ID on success, negative error code on failure.
 */
int ove_sim_debug_register(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_DEBUG_H */
