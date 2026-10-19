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
 * When CONFIG_OVE_PROFILER is enabled, a backend-specific sampler runs
 * at CONFIG_OVE_PROFILER_HZ and records up to
 * CONFIG_OVE_PROFILER_MAX_DEPTH program counters per sample into a
 * lock-free ring: the interrupted thread on on-CPU backends, every
 * thread on wall-clock ones (see OVE_PROFILER_FLAG_WALL_CLOCK). The
 * sim_profiler plugin drains the ring and forwards samples
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

/**
 * @brief Profiler flag: wall-clock sampling.
 *
 * Set when the backend samples every thread, including blocked ones, on
 * each tick (NuttX). Clear when it samples only the thread that was on the
 * CPU when the sampling interrupt fired (FreeRTOS, Zephyr). Reported to
 * the dashboard in the capabilities event.
 */
#define OVE_PROFILER_FLAG_WALL_CLOCK (1u << 0)

#ifdef CONFIG_OVE_PROFILER

/**
 * @brief Arm the backend sampling mechanism (a tick hook or timer,
 *        depending on the RTOS).
 *
 * Idempotent.  Sampling then runs on its own (tick hook, timer
 * interrupt or timer-queued work); the sim-debug pump only drains the
 * sample ring.
 *
 * @return OVE_OK on success, negative ove error code on failure.
 */
int ove_backend_profiler_start(void);

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
 * @brief Report how the backend samples.
 *
 * @return Bitwise OR of @c OVE_PROFILER_FLAG_* values.
 */
uint32_t ove_backend_profiler_get_flags(void);

#else

static inline int ove_backend_profiler_start(void)
{
	return 0;
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
static inline uint32_t ove_backend_profiler_get_flags(void)
{
	return 0;
}

#endif /* CONFIG_OVE_PROFILER */

#ifdef __cplusplus
}
#endif

#endif /* OVE_PROFILER_H */
