/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file pm.hpp
 * @brief Power management framework — state machine, wake sources,
 *        peripheral domains, and runtime statistics.
 */

#pragma once

#include <ove/pm.h>
#include <ove/types.hpp>

#ifdef CONFIG_OVE_PM

namespace ove
{

/**
 * @namespace ove::pm
 * @brief Thin C++ wrappers around the oveRTOS power management API.
 *
 * The PM subsystem is a singleton — there is one system-wide power state.
 * Available when `CONFIG_OVE_PM` is enabled.
 */
namespace pm
{

/* ── Enums (re-export C types for convenience) ──────────────────────── */

/** @brief System power state (active / sleep / deep-sleep / off). */
using State = ove_pm_state_t;
/** @brief Wake-source kind (GPIO, RTC, timer, …). */
using WakeType = ove_pm_wake_type_t;
/** @brief Peripheral power domain identifier. */
using Domain = ove_pm_domain_t;
/** @brief PM event delivered to subscribers (entering/exiting a state, etc.). */
using Event = ove_pm_event_t;
/** @brief Wake-source descriptor passed to `enable_wake_src()`. */
using WakeSrc = ove_pm_wake_src;
/** @brief Runtime configuration consumed by `init()`. */
using Cfg = ove_pm_cfg;
/** @brief Aggregated runtime statistics (time in each state, etc.). */
using Stats = ove_pm_stats;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/**
 * @brief Initialise the PM subsystem.
 * @param[in] cfg Configuration parameters.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int init(const Cfg &cfg)
{
	return ove_pm_init(&cfg);
}

/**
 * @brief Tear down the PM subsystem and release resources.
 */
inline void deinit()
{
	ove_pm_deinit();
}

/* ── State machine ──────────────────────────────────────────────────── */

/**
 * @brief Request an explicit power state transition.
 * @param[in] state Target power state.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int set_state(State state)
{
	return ove_pm_set_state(state);
}

/**
 * @brief Query the current power state.
 * @return Current power state.
 */
inline State get_state()
{
	return ove_pm_get_state();
}

/**
 * @brief Report system activity (ISR-safe).
 *
 * Resets the idle timer, causing the PM subsystem to return to the
 * ACTIVE state on the next idle check.
 */
inline void activity()
{
	ove_pm_activity();
}

/* ── Wake sources ───────────────────────────────────────────────────── */

/**
 * @brief Register a wake source.
 * @param[in] src Wake source descriptor.
 * @return `OVE_OK` on success, `OVE_ERR_NO_MEMORY` if table full.
 */
[[nodiscard]] inline int wake_register(const WakeSrc &src)
{
	return ove_pm_wake_register(&src);
}

/**
 * @brief Unregister a previously registered wake source.
 * @param[in] src Wake source descriptor (must match a registered entry).
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int wake_unregister(const WakeSrc &src)
{
	return ove_pm_wake_unregister(&src);
}

/* ── Peripheral power domains ───────────────────────────────────────── */

/**
 * @brief Increment the reference count for a peripheral power domain.
 * @param[in] domain Domain identifier.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int domain_request(Domain domain)
{
	return ove_pm_domain_request(domain);
}

/**
 * @brief Decrement the reference count for a peripheral power domain.
 * @param[in] domain Domain identifier.
 * @return `OVE_OK` on success, `OVE_ERR_INVALID_PARAM` on underflow.
 */
[[nodiscard]] inline int domain_release(Domain domain)
{
	return ove_pm_domain_release(domain);
}

/**
 * @brief Query the current reference count for a domain.
 * @param[in] domain Domain identifier.
 * @return Reference count (>= 0), or a negative error code.
 */
[[nodiscard]] inline int domain_get_refcount(Domain domain)
{
	return ove_pm_domain_get_refcount(domain);
}

/* ── Policy ─────────────────────────────────────────────────────────── */

/**
 * @brief Register a custom power policy callback.
 * @param[in] policy Policy function, or nullptr for default.
 * @param[in] user_data Opaque pointer forwarded to the policy.
 * @return `OVE_OK`.
 */
[[nodiscard]] inline int set_policy(ove_pm_policy_fn policy, void *user_data = nullptr)
{
	return ove_pm_set_policy(policy, user_data);
}

/* ── Notifications ──────────────────────────────────────────────────── */

/**
 * @brief Register a transition notification callback.
 * @param[in] cb Callback invoked on PRE_SLEEP and POST_WAKE.
 * @param[in] user_data Opaque pointer forwarded to the callback.
 * @return `OVE_OK` on success, `OVE_ERR_NO_MEMORY` if table full.
 */
[[nodiscard]] inline int notify_register(ove_pm_notify_fn cb, void *user_data = nullptr)
{
	return ove_pm_notify_register(cb, user_data);
}

/**
 * @brief Unregister a transition notification callback.
 * @param[in] cb Previously registered callback.
 * @param[in] user_data Pointer that was passed at registration time.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int notify_unregister(ove_pm_notify_fn cb, void *user_data = nullptr)
{
	return ove_pm_notify_unregister(cb, user_data);
}

/* ── Statistics ─────────────────────────────────────────────────────── */

/**
 * @brief Query accumulated power statistics.
 * @param[out] stats Receives current statistics snapshot.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int get_stats(Stats &stats)
{
	return ove_pm_get_stats(&stats);
}

/**
 * @brief Reset all accumulated power statistics to zero.
 */
inline void reset_stats()
{
	ove_pm_reset_stats();
}

/* ── Power budget ───────────────────────────────────────────────────── */

/**
 * @brief Set a target percentage of time in low-power states.
 * @param[in] target_pct_x100 Target in hundredths of percent.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int set_budget(uint32_t target_pct_x100)
{
	return ove_pm_set_budget(target_pct_x100);
}

/**
 * @brief Query actual low-power percentage vs. budget target.
 * @param[out] actual_pct_x100 Actual low-power % in hundredths.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int get_budget_status(uint32_t &actual_pct_x100)
{
	return ove_pm_get_budget_status(&actual_pct_x100);
}

/* ── Idle processing ────────────────────────────────────────────────── */

/**
 * @brief Process idle — drive the PM state machine.
 *
 * Called internally from the RTOS idle hook.  Not normally called by
 * application code.
 */
inline void idle_process()
{
	ove_pm_idle_process();
}

} /* namespace pm */

} /* namespace ove */

#endif /* CONFIG_OVE_PM */
