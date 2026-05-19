/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file profiler.h
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
 * @brief Arm the backend sampling mechanism (install signal handler on
 *        POSIX, etc.).
 *
 * Idempotent.  Does not spawn any thread — the consolidated sim-debug
 * pump drives sampling by calling @ref ove_backend_profiler_sample_tick
 * on its schedule.
 *
 * @return OVE_OK on success, negative ove error code on failure.
 */
int ove_backend_profiler_start(void);

/**
 * @brief Drive one sampler tick.
 *
 * Called from the sim-debug pump at @c CONFIG_OVE_PROFILER_HZ.
 * Snapshots runnable threads and delivers the profiling signal to
 * each.  No-op if @ref ove_backend_profiler_start has not succeeded.
 */
void ove_backend_profiler_sample_tick(void);

/**
 * @brief Disarm the backend sampling mechanism.
 *
 * Safe to call from any context.
 */
void ove_backend_profiler_stop(void);

/**
 * @brief Set the desired sampling rate in Hz.
 *
 * The compile-time @c CONFIG_OVE_PROFILER_HZ is the ceiling; @p hz is
 * silently clamped into [1, max].  Thread-safe.  The dashboard changes
 * rate by sending @c OVE_SIM_PROFILER_CMD_SET_RATE to the profiler
 * plugin, which then calls this.
 *
 * @param[in] hz Desired sampling rate in Hz.
 */
void ove_backend_profiler_set_rate(uint32_t hz);

/**
 * @brief Report the compile-time max sampling rate.
 *
 * Lets the dashboard know the ceiling without parsing Kconfig.
 *
 * @return Maximum supported sampling rate in Hz.
 */
uint32_t ove_backend_profiler_get_max_hz(void);

/**
 * @brief Drain newly-interned symbol entries as a JSON array.
 *
 * Writes a JSON array of @c [pc_start,pc_end,"name"] triples into
 * @p out, compatible with the dashboard's existing
 * @c PROFILE_SUB_SYMBOLS parser.
 *
 * Used by backends that symbolicate on-target (e.g. WASM uses
 * @c emscripten_get_callstack and interns names to synthetic
 * pseudo-PCs).  POSIX returns 0 because symbolication is done host-side
 * by the bridge via @c nm on the ELF.
 *
 * @param[out] out     Buffer to receive the JSON fragment.
 * @param[in]  out_max Size of @p out in bytes.
 * @return Bytes written to @p out (0 if nothing to emit or @p out_max
 *         is too small).
 */
size_t ove_backend_profiler_drain_symbols(char *out, size_t out_max);

#else

static inline int ove_backend_profiler_start(void)
{
	return 0;
}
static inline void ove_backend_profiler_sample_tick(void)
{
}
static inline void ove_backend_profiler_stop(void)
{
}
static inline void ove_backend_profiler_set_rate(uint32_t hz)
{
	(void)hz;
}
static inline uint32_t ove_backend_profiler_get_max_hz(void)
{
	return 0;
}
static inline size_t ove_backend_profiler_drain_symbols(char *out, size_t out_max)
{
	(void)out;
	(void)out_max;
	return 0;
}

#endif /* CONFIG_OVE_PROFILER */

#ifdef __cplusplus
}
#endif

#endif /* OVE_PROFILER_H */
