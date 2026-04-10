/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_pm Power Management
 * @ingroup ove_core
 * @brief Unified power management with sleep state machine, peripheral
 *        power domains, wake source registration, and runtime statistics.
 *
 * The PM subsystem is a singleton module (like GPIO and console) — there
 * is exactly one system power state.  Initialise with ove_pm_init() and
 * tear down with ove_pm_deinit().
 *
 * Four sleep states are defined, ordered by increasing depth:
 * - @c OVE_PM_STATE_ACTIVE   — full speed, all clocks running.
 * - @c OVE_PM_STATE_IDLE     — light sleep, fast wakeup, peripherals on.
 * - @c OVE_PM_STATE_STANDBY  — deeper sleep, some peripherals off.
 * - @c OVE_PM_STATE_DEEP_SLEEP — lowest power, RAM retained, slow wakeup.
 *
 * A pluggable policy engine decides when to transition between states
 * based on idle duration, next scheduled timeout, and registered wake
 * sources.  Peripheral power domains are reference-counted: power is
 * gated when the last user releases a domain.
 *
 * @note Requires @c CONFIG_OVE_PM.
 * @{
 */

#ifndef OVE_PM_H
#define OVE_PM_H

#include "ove/types.h"
#include "ove/gpio.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Enumerations ───────────────────────────────────────────────────── */

/**
 * @brief System power states, ordered by increasing sleep depth.
 */
typedef enum {
	OVE_PM_STATE_ACTIVE     = 0, /**< Full speed. */
	OVE_PM_STATE_IDLE       = 1, /**< Light sleep, fast wakeup. */
	OVE_PM_STATE_STANDBY    = 2, /**< Deep idle, some peripherals off. */
	OVE_PM_STATE_DEEP_SLEEP = 3, /**< Lowest power, RAM retained. */
	OVE_PM_STATE_COUNT      = 4,
} ove_pm_state_t;

/**
 * @brief Wake source types.
 */
typedef enum {
	OVE_PM_WAKE_GPIO  = 0, /**< GPIO pin edge. */
	OVE_PM_WAKE_TIMER = 1, /**< Timer expiry. */
	OVE_PM_WAKE_UART  = 2, /**< UART RX activity. */
	OVE_PM_WAKE_RTC   = 3, /**< RTC alarm. */
} ove_pm_wake_type_t;

/**
 * @brief Peripheral power domain identifiers.
 */
typedef enum {
	OVE_PM_DOMAIN_RADIO   = 0,
	OVE_PM_DOMAIN_SENSOR  = 1,
	OVE_PM_DOMAIN_DISPLAY = 2,
	OVE_PM_DOMAIN_AUDIO   = 3,
	OVE_PM_DOMAIN_STORAGE = 4,
	OVE_PM_DOMAIN_COMMS   = 5,
	OVE_PM_DOMAIN_USER0   = 6,
	OVE_PM_DOMAIN_USER1   = 7,
	OVE_PM_DOMAIN_COUNT   = 8,
} ove_pm_domain_t;

/**
 * @brief Transition event type for notification callbacks.
 */
typedef enum {
	OVE_PM_EVENT_PRE_SLEEP = 0, /**< About to enter low-power state. */
	OVE_PM_EVENT_POST_WAKE = 1, /**< Just woke from low-power state. */
} ove_pm_event_t;

/* ── Structures ─────────────────────────────────────────────────────── */

/**
 * @brief PM subsystem configuration.
 */
struct ove_pm_cfg {
	uint32_t idle_threshold_ms;       /**< Idle ms before ACTIVE→IDLE. */
	uint32_t standby_threshold_ms;    /**< Idle ms before IDLE→STANDBY. */
	uint32_t deep_sleep_threshold_ms; /**< Idle ms before →DEEP_SLEEP. */
};

/**
 * @brief Wake source descriptor.
 */
struct ove_pm_wake_src {
	ove_pm_wake_type_t type; /**< Type of wake source. */
	union {
		struct {
			unsigned int port;
			unsigned int pin;
			ove_gpio_irq_mode_t edge;
		} gpio;               /**< GPIO wake config. */
		struct {
			uint32_t timeout_ms;
		} timer;              /**< Timer wake config. */
		struct {
			unsigned int instance;
		} uart;               /**< UART wake config. */
		struct {
			uint32_t alarm_ms;
		} rtc;                /**< RTC wake config. */
	};
};

/**
 * @brief Runtime power statistics.
 */
struct ove_pm_stats {
	uint64_t time_in_state_us[OVE_PM_STATE_COUNT]; /**< Cumulative time per state. */
	uint32_t transition_count[OVE_PM_STATE_COUNT];  /**< Entries per state. */
	uint64_t total_runtime_us;                      /**< Total tracked time. */
	uint32_t active_pct_x100;                       /**< Active % in hundredths. */
};

/* ── Callback types ─────────────────────────────────────────────────── */

/**
 * @brief Power policy callback — returns recommended next state.
 *
 * @param[in] current         Current power state.
 * @param[in] idle_ms         Milliseconds since last activity.
 * @param[in] next_timeout_ms Milliseconds until next scheduled event.
 * @param[in] user_data       Opaque pointer from ove_pm_set_policy().
 * @return Recommended next power state.
 */
typedef ove_pm_state_t (*ove_pm_policy_fn)(ove_pm_state_t current,
					   uint32_t idle_ms,
					   uint32_t next_timeout_ms,
					   void *user_data);

/**
 * @brief Transition notification callback.
 *
 * @param[in] event      PRE_SLEEP or POST_WAKE.
 * @param[in] from_state State being left.
 * @param[in] to_state   State being entered.
 * @param[in] user_data  Opaque pointer from ove_pm_notify_register().
 */
typedef void (*ove_pm_notify_fn)(ove_pm_event_t event,
				 ove_pm_state_t from_state,
				 ove_pm_state_t to_state,
				 void *user_data);

/* ── Public API ─────────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_PM

/**
 * @brief Initialise the PM subsystem.
 *
 * @param[in] cfg  Configuration parameters.  Must not be NULL.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_pm_init(const struct ove_pm_cfg *cfg);

/**
 * @brief Tear down the PM subsystem and release resources.
 */
void ove_pm_deinit(void);

/**
 * @brief Request an explicit transition to @p state.
 *
 * @param[in] state  Target power state.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_pm_set_state(ove_pm_state_t state);

/**
 * @brief Query the current power state.
 *
 * @return Current power state.
 */
ove_pm_state_t ove_pm_get_state(void);

/**
 * @brief Report system activity (resets idle timer).
 *
 * This function is ISR-safe — it performs only a volatile write.
 */
void ove_pm_activity(void);

/**
 * @brief Register a wake source.
 *
 * @param[in] src  Wake source descriptor.
 * @return OVE_OK on success, OVE_ERR_NO_MEMORY if table full.
 */
int ove_pm_wake_register(const struct ove_pm_wake_src *src);

/**
 * @brief Unregister a previously registered wake source.
 *
 * @param[in] src  Wake source descriptor (must match a registered entry).
 * @return OVE_OK on success, OVE_ERR_NOT_REGISTERED if not found.
 */
int ove_pm_wake_unregister(const struct ove_pm_wake_src *src);

/**
 * @brief Increment the reference count for a peripheral power domain.
 *
 * On the first request (0→1), the domain hardware is powered on.
 *
 * @param[in] domain  Domain identifier.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_pm_domain_request(ove_pm_domain_t domain);

/**
 * @brief Decrement the reference count for a peripheral power domain.
 *
 * When the count reaches zero, the domain hardware is powered off.
 *
 * @param[in] domain  Domain identifier.
 * @return OVE_OK on success, OVE_ERR_INVALID_PARAM on underflow.
 */
int ove_pm_domain_release(ove_pm_domain_t domain);

/**
 * @brief Query the current reference count for a domain.
 *
 * @param[in] domain  Domain identifier.
 * @return Reference count (>= 0), or negative error code.
 */
int ove_pm_domain_get_refcount(ove_pm_domain_t domain);

/**
 * @brief Register a custom power policy callback.
 *
 * Replaces the default threshold-based policy.  Pass NULL to restore
 * the default policy.
 *
 * @param[in] policy    Policy function, or NULL for default.
 * @param[in] user_data Opaque pointer forwarded to @p policy.
 * @return OVE_OK.
 */
int ove_pm_set_policy(ove_pm_policy_fn policy, void *user_data);

/**
 * @brief Register a transition notification callback.
 *
 * @param[in] cb        Callback invoked on PRE_SLEEP and POST_WAKE.
 * @param[in] user_data Opaque pointer forwarded to @p cb.
 * @return OVE_OK on success, OVE_ERR_NO_MEMORY if table full.
 */
int ove_pm_notify_register(ove_pm_notify_fn cb, void *user_data);

/**
 * @brief Unregister a transition notification callback.
 *
 * @param[in] cb        Previously registered callback.
 * @param[in] user_data Pointer that was passed at registration time.
 * @return OVE_OK on success, OVE_ERR_NOT_REGISTERED if not found.
 */
int ove_pm_notify_unregister(ove_pm_notify_fn cb, void *user_data);

/**
 * @brief Query accumulated power statistics.
 *
 * @param[out] stats  Receives current statistics snapshot.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_pm_get_stats(struct ove_pm_stats *stats);

/**
 * @brief Reset all accumulated power statistics to zero.
 */
void ove_pm_reset_stats(void);

/**
 * @brief Set a target percentage of time in low-power states.
 *
 * @param[in] target_low_power_pct_x100  Target in hundredths of percent.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_pm_set_budget(uint32_t target_low_power_pct_x100);

/**
 * @brief Query actual low-power percentage vs. budget target.
 *
 * @param[out] actual_pct_x100  Actual low-power % in hundredths.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_pm_get_budget_status(uint32_t *actual_pct_x100);

/**
 * @brief Process idle — called from RTOS idle context by the HAL.
 *
 * This drives the state machine: checks activity, invokes policy,
 * arms wake sources, transitions state, fires notifications.
 * Not intended to be called by application code.
 */
void ove_pm_idle_process(void);

#else /* !CONFIG_OVE_PM */

static inline int            ove_pm_init(const struct ove_pm_cfg *c) { (void)c; return OVE_ERR_NOT_SUPPORTED; }
static inline void           ove_pm_deinit(void) {}
static inline int            ove_pm_set_state(ove_pm_state_t s) { (void)s; return OVE_ERR_NOT_SUPPORTED; }
static inline ove_pm_state_t ove_pm_get_state(void) { return OVE_PM_STATE_ACTIVE; }
static inline void           ove_pm_activity(void) {}
static inline int            ove_pm_wake_register(const struct ove_pm_wake_src *s) { (void)s; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_wake_unregister(const struct ove_pm_wake_src *s) { (void)s; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_domain_request(ove_pm_domain_t d) { (void)d; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_domain_release(ove_pm_domain_t d) { (void)d; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_domain_get_refcount(ove_pm_domain_t d) { (void)d; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_set_policy(ove_pm_policy_fn p, void *u) { (void)p; (void)u; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_notify_register(ove_pm_notify_fn c, void *u) { (void)c; (void)u; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_notify_unregister(ove_pm_notify_fn c, void *u) { (void)c; (void)u; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_get_stats(struct ove_pm_stats *s) { (void)s; return OVE_ERR_NOT_SUPPORTED; }
static inline void           ove_pm_reset_stats(void) {}
static inline int            ove_pm_set_budget(uint32_t t) { (void)t; return OVE_ERR_NOT_SUPPORTED; }
static inline int            ove_pm_get_budget_status(uint32_t *a) { (void)a; return OVE_ERR_NOT_SUPPORTED; }
static inline void           ove_pm_idle_process(void) {}

#endif /* CONFIG_OVE_PM */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_pm group */

#endif /* OVE_PM_H */
