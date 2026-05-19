/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file watchdog.h
 * @defgroup ove_watchdog Watchdog
 * @ingroup ove_core
 * @brief Hardware or software watchdog timer control.
 *
 * Provides a portable interface for arming, servicing, and disarming a
 * watchdog timer. If the watchdog is not fed within @p timeout_ms milliseconds
 * after being started, the system will reset (or invoke the backend's
 * expiry action).
 *
 * Two allocation strategies are supported:
 * - @c _create() / @c _destroy() — heap-allocated.  Available only when
 *   @c OVE_HEAP_WATCHDOG is defined (i.e. @c CONFIG_OVE_ZERO_HEAP is not set).
 * - @c _init() / @c _deinit() — caller-supplied storage.  Available in both
 *   modes.  See @c OVE_WATCHDOG_DEFINE_STATIC for a one-step static helper.
 *
 * @note Requires @c CONFIG_OVE_WATCHDOG.
 * @{
 */

#ifndef OVE_WATCHDOG_H
#define OVE_WATCHDOG_H

#include "ove/types.h"
#include "ove_config.h"
#include "ove/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_WATCHDOG

/**
 * @brief Initialise a watchdog timer using caller-provided static storage.
 *
 * Configures the watchdog with a timeout of @p timeout_ms milliseconds.
 * The watchdog does not start counting until @ref ove_watchdog_start is
 * called. The caller must ensure @p storage remains valid for the watchdog's
 * lifetime.
 *
 * @param[out] wdt         Receives the initialised watchdog handle.
 * @param[in]  storage     Pointer to statically-allocated watchdog storage.
 * @param[in]  timeout_ms  Watchdog timeout period in milliseconds.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WATCHDOG.
 */
int ove_watchdog_init(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage, uint32_t timeout_ms);

/**
 * @brief Deinitialise a statically-allocated watchdog timer.
 *
 * Stops the watchdog if running and releases all associated RTOS resources.
 * The backing storage memory is not freed.
 *
 * @param[in] wdt  Watchdog handle returned by @ref ove_watchdog_init.
 * @note Requires @c CONFIG_OVE_WATCHDOG.
 */
void ove_watchdog_deinit(ove_watchdog_t wdt);

/**
 * @brief Allocate and initialise a heap-backed watchdog timer.
 *
 * Configures the watchdog with a timeout of @p timeout_ms milliseconds.
 * The watchdog does not start counting until @ref ove_watchdog_start is
 * called. The returned handle must be freed with @ref ove_watchdog_destroy.
 *
 * @param[out] wdt         Receives the created watchdog handle.
 * @param[in]  timeout_ms  Watchdog timeout period in milliseconds.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WATCHDOG and @c OVE_HEAP_WATCHDOG.
 */
#ifdef OVE_HEAP_WATCHDOG
int ove_watchdog_create(ove_watchdog_t *wdt, uint32_t timeout_ms);

/**
 * @brief Destroy a heap-allocated watchdog timer.
 *
 * Stops the watchdog if running and frees all resources. Must only be called
 * on handles obtained from @ref ove_watchdog_create.
 *
 * @param[in] wdt  Watchdog handle returned by @ref ove_watchdog_create.
 * @note Requires @c CONFIG_OVE_WATCHDOG and @c OVE_HEAP_WATCHDOG.
 */
void ove_watchdog_destroy(ove_watchdog_t wdt);
#endif /* OVE_HEAP_WATCHDOG */

/**
 * @brief Start (arm) the watchdog timer.
 *
 * Begins the countdown. The watchdog must be fed with @ref ove_watchdog_feed
 * at intervals shorter than the configured timeout, or the system will reset.
 *
 * @param[in] wdt  Watchdog handle.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WATCHDOG.
 */
int ove_watchdog_start(ove_watchdog_t wdt);

/**
 * @brief Stop (disarm) the watchdog timer.
 *
 * Halts the countdown. The watchdog will not reset the system while stopped.
 * Call @ref ove_watchdog_start to re-arm.
 *
 * @param[in] wdt  Watchdog handle.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WATCHDOG.
 */
int ove_watchdog_stop(ove_watchdog_t wdt);

/**
 * @brief Feed (pet) the watchdog to prevent a system reset.
 *
 * Resets the watchdog countdown to the configured timeout. Must be called
 * periodically while the watchdog is running, with an interval shorter than
 * the @p timeout_ms value passed to @ref ove_watchdog_init or
 * @ref ove_watchdog_create.
 *
 * @param[in] wdt  Watchdog handle.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WATCHDOG.
 */
int ove_watchdog_feed(ove_watchdog_t wdt);

#else /* !CONFIG_OVE_WATCHDOG */

/* P0-3: _init/_deinit stubs so OVE_WATCHDOG_DEFINE_STATIC links cleanly
 * when CONFIG_OVE_WATCHDOG=n. */
static inline int ove_watchdog_init(ove_watchdog_t *w, ove_watchdog_storage_t *s, uint32_t t)
{
	(void)w;
	(void)s;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_watchdog_deinit(ove_watchdog_t w)
{
	(void)w;
}

static inline int ove_watchdog_create(ove_watchdog_t *w, uint32_t t)
{
	(void)w;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_watchdog_destroy(ove_watchdog_t w)
{
	(void)w;
}
static inline int ove_watchdog_start(ove_watchdog_t w)
{
	(void)w;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_watchdog_stop(ove_watchdog_t w)
{
	(void)w;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_watchdog_feed(ove_watchdog_t w)
{
	(void)w;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_WATCHDOG */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_watchdog group */

#endif /* OVE_WATCHDOG_H */
