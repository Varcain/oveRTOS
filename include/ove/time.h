/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_time Time
 * @ingroup ove_core
 * @brief High-resolution timestamps and blocking delay utilities.
 *
 * Provides monotonic time queries at microsecond and nanosecond resolution,
 * as well as blocking delay functions for both millisecond and microsecond
 * granularities.
 *
 * Timestamp values are monotonically non-decreasing from an arbitrary epoch
 * (typically system boot). The actual resolution is backend-dependent and
 * may be coarser than the unit implies on some platforms.
 *
 * @note Requires @c CONFIG_OVE_TIME.
 * @{
 */

#ifndef OVE_TIME_H
#define OVE_TIME_H

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_TIME

/**
 * @brief Get the current monotonic time in microseconds.
 *
 * Reads the system timer and writes the elapsed microseconds since an
 * arbitrary epoch (typically boot) to @p out.
 *
 * @param[out] out  Receives the current timestamp in microseconds.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_TIME.
 */
int ove_time_get_us(uint64_t *out);

/**
 * @brief Get the current monotonic time in nanoseconds.
 *
 * Reads the system timer and writes the elapsed nanoseconds since an
 * arbitrary epoch (typically boot) to @p out. Actual nanosecond resolution
 * depends on the hardware timer; values may be rounded to the nearest tick.
 *
 * @param[out] out  Receives the current timestamp in nanoseconds.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_TIME.
 */
int ove_time_get_ns(uint64_t *out);

/**
 * @brief Block the calling task for at least @p ms milliseconds.
 *
 * Suspends the current task for a minimum of @p ms milliseconds. The actual
 * sleep duration may be longer due to scheduling granularity and system load.
 * Passing 0 yields the CPU to any equal or higher-priority task.
 *
 * @param[in] ms  Minimum delay in milliseconds.
 * @note Requires @c CONFIG_OVE_TIME. Must not be called from an ISR.
 */
void ove_time_delay_ms(uint32_t ms);

/**
 * @brief Block the calling task for at least @p us microseconds.
 *
 * Suspends the current task for a minimum of @p us microseconds. For very
 * short delays the implementation may busy-wait rather than yield,
 * depending on the RTOS tick resolution.
 *
 * @param[in] us  Minimum delay in microseconds.
 * @note Requires @c CONFIG_OVE_TIME. Must not be called from an ISR.
 */
void ove_time_delay_us(uint32_t us);

#else /* !CONFIG_OVE_TIME */

static inline int ove_time_get_us(uint64_t *o)
{
	if (o) {
		*o = 0;
	}
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_time_get_ns(uint64_t *o)
{
	if (o) {
		*o = 0;
	}
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_time_delay_ms(uint32_t ms)
{
	(void)ms;
}
static inline void ove_time_delay_us(uint32_t us)
{
	(void)us;
}

#endif /* CONFIG_OVE_TIME */

/* ── Duration helper macros (ns-resolution) ────────────────────────────
 *
 * Express timeouts for the upcoming uint64_t ns APIs ergonomically:
 *   ove_mutex_lock_ns(m, OVE_MS(100))
 *   ove_queue_send_ns(q, &item, OVE_SEC(5))
 *
 * All multiplications fold at compile time for constant inputs.
 * Available in both CONFIG_OVE_TIME modes — the macros don't call into
 * the time backend.
 */
#define OVE_NS(n)  ((uint64_t)(n))
#define OVE_US(n)  ((uint64_t)(n) * 1000ULL)
#define OVE_MS(n)  ((uint64_t)(n) * 1000000ULL)
#define OVE_SEC(n) ((uint64_t)(n) * 1000000000ULL)
#define OVE_MIN(n) (OVE_SEC(n) * 60ULL)

/**
 * @brief Get the current monotonic time in nanoseconds (value-return form).
 *
 * Convenience wrapper over @ref ove_time_get_ns with a friendlier
 * signature. Failure paths return 0; on every backend supported today
 * the underlying call cannot fail in practice.
 *
 * @return Monotonic timestamp in nanoseconds since an arbitrary epoch.
 */
static inline uint64_t ove_time_now_steady_ns(void)
{
	uint64_t out = 0;
	int rc = ove_time_get_ns(&out);
	(void)rc;
	return out;
}

/**
 * @brief Convert a steady-clock deadline to a duration suitable for the
 *        existing @c timeout_ns-taking APIs.
 *
 * If @p deadline_ns is in the past relative to @ref ove_time_now_steady_ns,
 * returns 0 (the caller should not block).  The @c OVE_WAIT_FOREVER sentinel
 * is preserved verbatim so @c _until shims can pass it straight through
 * without altering the indefinite-block semantics.
 *
 * @param[in] deadline_ns Absolute deadline in nanoseconds from the same
 *                        epoch as @ref ove_time_now_steady_ns.
 * @return Nanoseconds remaining until the deadline, 0 if the deadline has
 *         passed, or @c OVE_WAIT_FOREVER if @p deadline_ns is the sentinel.
 */
static inline uint64_t ove_time_deadline_to_timeout_ns(uint64_t deadline_ns)
{
	if (deadline_ns == OVE_WAIT_FOREVER)
		return OVE_WAIT_FOREVER;
	uint64_t now = ove_time_now_steady_ns();
	return (deadline_ns > now) ? (deadline_ns - now) : 0;
}

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_time group */

#endif /* OVE_TIME_H */
