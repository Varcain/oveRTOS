/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @brief Sampling profiler — periodic PC + call-stack capture.
 *
 * When CONFIG_OVE_PROFILER is enabled, a backend-specific sampler
 * interrupts each RUNNING thread at CONFIG_OVE_PROFILER_HZ and records
 * up to CONFIG_OVE_PROFILER_MAX_DEPTH program counters into a lock-free
 * ring. The sim_profiler plugin drains the ring and forwards samples
 * to the dashboard, which aggregates them into a flat top-N table and
 * a flame graph.
 *
 * Backends provide:
 *   - ove_backend_profiler_start() — arm the sampling mechanism
 *   - ove_backend_profiler_stop()  — tear it down
 *
 * The ring API (ove_profiler_ring.h) is shared.
 */

#ifndef OVE_PROFILER_H
#define OVE_PROFILER_H

#include <stddef.h>
#include <stdint.h>

#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_PROFILER

/**
 * Arm the backend sampling mechanism (install signal handler on POSIX,
 * etc.). Idempotent. Does not spawn any thread — the consolidated
 * sim-debug pump drives sampling by calling ove_backend_profiler_sample_tick()
 * on its schedule. Returns 0 on success, negative ove error code on failure.
 */
int ove_backend_profiler_start(void);

/**
 * Drive one sampler tick. Called from the sim-debug pump at
 * CONFIG_OVE_PROFILER_HZ. Snapshots runnable threads and delivers the
 * profiling signal to each. No-op if start() has not succeeded.
 */
void ove_backend_profiler_sample_tick(void);

/**
 * Disarm the backend sampling mechanism. Safe to call from any context.
 */
void ove_backend_profiler_stop(void);

/**
 * Set the desired sampling rate in Hz. The compile-time CONFIG_OVE_PROFILER_HZ
 * is the ceiling; @p hz is silently clamped into [1, max]. Thread-safe.
 * Dashboard changes rate by sending OVE_SIM_PROFILER_CMD_SET_RATE to the
 * profiler plugin, which then calls this.
 */
void ove_backend_profiler_set_rate(uint32_t hz);

/**
 * Report the compile-time max sampling rate so the dashboard knows the
 * ceiling without parsing Kconfig.
 */
uint32_t ove_backend_profiler_get_max_hz(void);

#else

static inline int  ove_backend_profiler_start(void) { return 0; }
static inline void ove_backend_profiler_sample_tick(void) { }
static inline void ove_backend_profiler_stop(void)  { }
static inline void ove_backend_profiler_set_rate(uint32_t hz) { (void)hz; }
static inline uint32_t ove_backend_profiler_get_max_hz(void) { return 0; }

#endif /* CONFIG_OVE_PROFILER */

#ifdef __cplusplus
}
#endif

#endif /* OVE_PROFILER_H */
