/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file timer.h
 * @defgroup ove_timer Software timer
 * @brief Periodic and one-shot software timer API backed by the active RTOS.
 *
 * @note All functions in this group require @c CONFIG_OVE_TIMER to be defined.
 *       When @c CONFIG_OVE_TIMER is not set, every function is replaced by a
 *       static inline stub that returns @c OVE_ERR_NOT_SUPPORTED.
 *
 * Two allocation strategies are available:
 *  - @c _create() / @c _destroy() — heap-allocated.  Available only when
 *    @c OVE_HEAP_TIMER is defined (i.e. @c CONFIG_OVE_ZERO_HEAP is not set).
 *  - @c _init() / @c _deinit() — caller-supplied storage.  Available in both
 *    modes.  See @c OVE_TIMER_DEFINE_STATIC for a one-step static helper.
 * @{
 */

#ifndef OVE_TIMER_H
#define OVE_TIMER_H

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle for a software timer object. */
typedef struct ove_timer *ove_timer_t;

/**
 * @brief Timer expiry callback function prototype.
 *
 * Invoked by the RTOS timer service task (or equivalent) when the timer
 * period elapses.  Implementations must be non-blocking and short.
 *
 * @param[in] timer      Handle of the timer that fired.
 * @param[in] user_data  Opaque pointer supplied at timer creation time.
 */
typedef void (*ove_timer_fn)(ove_timer_t timer, void *user_data);

#include "ove/storage.h"

#ifdef CONFIG_OVE_TIMER

/**
 * @brief Initialise a software timer using caller-supplied static storage.
 *
 * Creates a timer in the stopped state.  Call ove_timer_start() to arm it.
 *
 * @note Requires @c CONFIG_OVE_TIMER.
 *
 * @param[out] timer      Receives the opaque timer handle on success.
 * @param[in]  storage    Pointer to statically allocated backend storage.
 *                        Must remain valid for the lifetime of the timer.
 * @param[in]  callback   Function invoked when the timer expires.
 *                        Must not be NULL.
 * @param[in]  user_data  Opaque pointer forwarded to @p callback on each
 *                        expiry.  May be NULL.
 * @param[in]  period_ms  Timer period in milliseconds.  Must be > 0.
 * @param[in]  one_shot   Non-zero to create a one-shot timer (fires once
 *                        then stops automatically); zero for a periodic
 *                        timer that reloads automatically.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_timer_deinit, ove_timer_create, ove_timer_start
 */
int ove_timer_init(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		   void *user_data, uint32_t period_ms, int one_shot);

/**
 * @brief Stop and release resources held by a timer initialised with
 *        ove_timer_init().
 *
 * Stops the timer if it is running.  The static storage supplied at init
 * time is not freed.
 *
 * @note Requires @c CONFIG_OVE_TIMER.
 *
 * @param[in] timer  Handle returned by ove_timer_init().
 *
 * @see ove_timer_init
 */
void ove_timer_deinit(ove_timer_t timer);

/**
 * @brief Initialise a software timer with a nanosecond period using
 *        caller-supplied static storage.
 *
 * Identical to @ref ove_timer_init except the period is specified in
 * nanoseconds.  Used primarily by the async runtime substrate
 * (@c CONFIG_OVE_ASYNC) to drive the Embassy time driver at sub-ms
 * granularity.
 *
 * Effective resolution is backend-dependent:
 *  - Zephyr:  underlying tick (K_NSEC); typically µs-class.
 *  - FreeRTOS: rounded up to @c configTICK_RATE_HZ; typically 1 ms.
 *  - NuttX:   POSIX timer resolution; typically µs.
 *  - POSIX:   @c timer_create with CLOCK_MONOTONIC; nsec.
 *
 * @note Requires @c CONFIG_OVE_TIMER.  When @c CONFIG_OVE_ASYNC is off,
 *       the function is still available — it just isn't routed by the
 *       async substrate.
 *
 * @param[out] timer      Receives the opaque timer handle on success.
 * @param[in]  storage    Pointer to statically allocated backend storage.
 * @param[in]  callback   Function invoked when the timer expires.
 *                        Must not be NULL.
 * @param[in]  user_data  Opaque pointer forwarded to @p callback.
 * @param[in]  period_ns  Timer period in nanoseconds.  Must be > 0.
 * @param[in]  one_shot   Non-zero for one-shot, zero for periodic.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_timer_init, ove_timer_create_ns
 */
int ove_timer_init_ns(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		      void *user_data, uint64_t period_ns, int one_shot);

/* _create / _destroy — heap-gated */
#ifdef OVE_HEAP_TIMER

/**
 * @brief Allocate and initialise a software timer from the heap.
 *
 * Creates a timer in the stopped state.  Call ove_timer_start() to arm it.
 *
 * @note Requires @c CONFIG_OVE_TIMER and @c OVE_HEAP_TIMER
 *       (i.e. @c CONFIG_OVE_ZERO_HEAP must not be set).
 *
 * @param[out] timer      Receives the opaque timer handle on success.
 * @param[in]  callback   Function invoked when the timer expires.
 *                        Must not be NULL.
 * @param[in]  user_data  Opaque pointer forwarded to @p callback on each
 *                        expiry.  May be NULL.
 * @param[in]  period_ms  Timer period in milliseconds.  Must be > 0.
 * @param[in]  one_shot   Non-zero to create a one-shot timer; zero for a
 *                        periodic auto-reloading timer.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_timer_destroy, ove_timer_init, ove_timer_start
 */
int ove_timer_create(ove_timer_t *timer, ove_timer_fn callback, void *user_data, uint32_t period_ms,
		     int one_shot);

/**
 * @brief Allocate and initialise a software timer with a nanosecond
 *        period from the heap.
 *
 * Identical to @ref ove_timer_create except the period is specified in
 * nanoseconds.  See @ref ove_timer_init_ns for the per-backend
 * resolution floor.
 *
 * @note Requires @c CONFIG_OVE_TIMER and @c OVE_HEAP_TIMER.
 *
 * @param[out] timer      Receives the opaque timer handle on success.
 * @param[in]  callback   Function invoked when the timer expires.
 *                        Must not be NULL.
 * @param[in]  user_data  Opaque pointer forwarded to @p callback.
 * @param[in]  period_ns  Timer period in nanoseconds.  Must be > 0.
 * @param[in]  one_shot   Non-zero for one-shot, zero for periodic.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_timer_create, ove_timer_init_ns
 */
int ove_timer_create_ns(ove_timer_t *timer, ove_timer_fn callback, void *user_data,
			uint64_t period_ns, int one_shot);

/**
 * @brief Stop and free a timer allocated with ove_timer_create().
 *
 * @note Requires @c CONFIG_OVE_TIMER and @c OVE_HEAP_TIMER.
 *
 * @param[in] timer  Handle returned by ove_timer_create().
 *
 * @see ove_timer_create
 */
void ove_timer_destroy(ove_timer_t timer);

#endif /* OVE_HEAP_TIMER */

/**
 * @brief Start (arm) a timer.
 *
 * If the timer is already running, it is restarted from the beginning of
 * its period.  Has no effect if the timer is in a terminated state.
 *
 * @note Requires @c CONFIG_OVE_TIMER.
 *
 * @param[in] timer  Timer handle to start.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_timer_stop, ove_timer_reset
 */
int ove_timer_start(ove_timer_t timer);

/**
 * @brief Stop a running timer without invoking its callback.
 *
 * If the timer is already stopped, this function has no effect.
 *
 * @note Requires @c CONFIG_OVE_TIMER.
 *
 * @param[in] timer  Timer handle to stop.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_timer_start, ove_timer_reset
 */
int ove_timer_stop(ove_timer_t timer);

/**
 * @brief Restart a timer's countdown from the beginning of its period.
 *
 * Equivalent to stopping and then starting the timer, but performed
 * atomically with respect to the RTOS timer service.  Useful for
 * implementing watchdog-style "kick" patterns.
 *
 * @note Requires @c CONFIG_OVE_TIMER.
 *
 * @param[in] timer  Timer handle to reset.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_timer_start, ove_timer_stop
 */
int ove_timer_reset(ove_timer_t timer);

#else /* !CONFIG_OVE_TIMER */

/* P0-3: _init/_deinit stubs so OVE_TIMER_DEFINE_STATIC links cleanly when
 * CONFIG_OVE_TIMER=n. */
static inline int ove_timer_init(ove_timer_t *t, ove_timer_storage_t *s, ove_timer_fn cb, void *ud,
				 uint32_t p, int os)
{
	(void)t;
	(void)s;
	(void)cb;
	(void)ud;
	(void)p;
	(void)os;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_timer_deinit(ove_timer_t t)
{
	(void)t;
}

static inline int ove_timer_create(ove_timer_t *t, ove_timer_fn cb, void *ud, uint32_t p, int os)
{
	(void)t;
	(void)cb;
	(void)ud;
	(void)p;
	(void)os;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_timer_init_ns(ove_timer_t *t, ove_timer_storage_t *s, ove_timer_fn cb,
				    void *ud, uint64_t p, int os)
{
	(void)t;
	(void)s;
	(void)cb;
	(void)ud;
	(void)p;
	(void)os;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_timer_create_ns(ove_timer_t *t, ove_timer_fn cb, void *ud, uint64_t p, int os)
{
	(void)t;
	(void)cb;
	(void)ud;
	(void)p;
	(void)os;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_timer_destroy(ove_timer_t t)
{
	(void)t;
}
static inline int ove_timer_start(ove_timer_t t)
{
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_timer_stop(ove_timer_t t)
{
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_timer_reset(ove_timer_t t)
{
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_TIMER */

#ifdef __cplusplus
}
#endif

#endif /* OVE_TIMER_H */

/** @} */
