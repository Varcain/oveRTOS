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
#include <ove/error.hpp>

#ifdef CONFIG_OVE_PM

namespace ove::pm
{

/**
 * @namespace ove::pm
 * @brief Thin C++ wrappers around the oveRTOS power management API.
 *
 * The PM subsystem is a singleton — there is one system-wide power state.
 * Available when `CONFIG_OVE_PM` is enabled.
 */

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
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> init(const Cfg &cfg) noexcept
{
	return from_rc(ove_pm_init(&cfg));
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
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> set_state(State state) noexcept
{
	return from_rc(ove_pm_set_state(state));
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
 * @return Empty `Result<void>` on success; `unexpected`
 *         @ref Error::NoMemory if the wake-source table is full.
 */
[[nodiscard]] inline Result<void> wake_register(const WakeSrc &src) noexcept
{
	return from_rc(ove_pm_wake_register(&src));
}

/**
 * @brief Unregister a previously registered wake source.
 * @param[in] src Wake source descriptor (must match a registered entry).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> wake_unregister(const WakeSrc &src) noexcept
{
	return from_rc(ove_pm_wake_unregister(&src));
}

/* ── Peripheral power domains ───────────────────────────────────────── */

/**
 * @brief Increment the reference count for a peripheral power domain.
 * @param[in] domain Domain identifier.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> domain_request(Domain domain) noexcept
{
	return from_rc(ove_pm_domain_request(domain));
}

/**
 * @brief Decrement the reference count for a peripheral power domain.
 * @param[in] domain Domain identifier.
 * @return Empty `Result<void>` on success; `unexpected`
 *         @ref Error::InvalidParam on refcount underflow.
 */
[[nodiscard]] inline Result<void> domain_release(Domain domain) noexcept
{
	return from_rc(ove_pm_domain_release(domain));
}

/**
 * @brief Query the current reference count for a domain.
 * @param[in] domain Domain identifier.
 * @return On success, the reference count.  On failure, an
 *         `unexpected` @ref Error.
 */
[[nodiscard]] inline Result<int> domain_get_refcount(Domain domain) noexcept
{
	const int rc = ove_pm_domain_get_refcount(domain);
	if (rc >= 0)
		return rc;
	return std::unexpected{static_cast<Error>(rc)};
}

/* ── Policy ─────────────────────────────────────────────────────────── */

/**
 * @brief Register a custom power policy callback.
 * @param[in] policy Policy function, or nullptr for default.
 * @param[in] user_data Opaque pointer forwarded to the policy.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> set_policy(ove_pm_policy_fn policy,
					     void *user_data = nullptr) noexcept
{
	return from_rc(ove_pm_set_policy(policy, user_data));
}

/* ── Notifications ──────────────────────────────────────────────────── */

/**
 * @brief Register a transition notification callback.
 * @param[in] cb Callback invoked on PRE_SLEEP and POST_WAKE.
 * @param[in] user_data Opaque pointer forwarded to the callback.
 * @return Empty `Result<void>` on success; `unexpected`
 *         @ref Error::NoMemory if the notification table is full.
 */
[[nodiscard]] inline Result<void> notify_register(ove_pm_notify_fn cb,
						  void *user_data = nullptr) noexcept
{
	return from_rc(ove_pm_notify_register(cb, user_data));
}

/**
 * @brief Unregister a transition notification callback.
 * @param[in] cb Previously registered callback.
 * @param[in] user_data Pointer that was passed at registration time.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> notify_unregister(ove_pm_notify_fn cb,
						    void *user_data = nullptr) noexcept
{
	return from_rc(ove_pm_notify_unregister(cb, user_data));
}

/* ── Statistics ─────────────────────────────────────────────────────── */

/**
 * @brief Query accumulated power statistics.
 * @return On success, the populated @ref Stats snapshot.  On
 *         failure, an `unexpected` @ref Error.
 */
[[nodiscard]] inline Result<Stats> get_stats() noexcept
{
	Stats stats{};
	const int rc = ove_pm_get_stats(&stats);
	return from_rc(rc, stats);
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
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> set_budget(uint32_t target_pct_x100) noexcept
{
	return from_rc(ove_pm_set_budget(target_pct_x100));
}

/**
 * @brief Query actual low-power percentage vs. budget target.
 * @return On success, the actual low-power percentage in hundredths
 *         of percent.  On failure, an `unexpected` @ref Error.
 */
[[nodiscard]] inline Result<uint32_t> get_budget_status() noexcept
{
	uint32_t actual_pct_x100 = 0;
	const int rc = ove_pm_get_budget_status(&actual_pct_x100);
	return from_rc(rc, actual_pct_x100);
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

} /* namespace ove::pm */

#endif /* CONFIG_OVE_PM */
