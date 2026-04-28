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

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_time group */

#endif /* OVE_TIME_H */
