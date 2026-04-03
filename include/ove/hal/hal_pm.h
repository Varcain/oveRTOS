/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_PM_H
#define OVE_HAL_PM_H

/**
 * @defgroup ove_hal_pm HAL Power Management Interface
 * @brief Hardware Abstraction Layer interface for power management.
 *
 * Declares the low-level PM functions that every platform HAL must
 * implement.  The portable @ref ove_pm layer delegates to these
 * functions after performing state machine logic, refcounting, and
 * statistics tracking.
 *
 * @note Platform implementations supply their own definitions of these
 *       functions in a backend-specific source file.
 * @{
 */

#include "ove/pm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enter a hardware sleep state.
 *
 * Called by the portable PM layer after arming wake sources and firing
 * PRE_SLEEP notifications.  The function blocks until the system wakes.
 *
 * @param[in] state            Target sleep state.
 * @param[in] expected_idle_ms Hint: expected sleep duration in ms.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms);

/**
 * @brief Arm a wake source in hardware before entering sleep.
 *
 * Called for each registered wake source before ove_hal_pm_enter_state().
 *
 * @param[in] src  Wake source descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_pm_wake_arm(const struct ove_pm_wake_src *src);

/**
 * @brief Disarm a wake source after waking.
 *
 * @param[in] src  Wake source descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_pm_wake_disarm(const struct ove_pm_wake_src *src);

/**
 * @brief Enable power to a peripheral domain.
 *
 * Called when a domain's reference count transitions from 0 to 1.
 *
 * @param[in] domain  Domain identifier.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_pm_domain_enable(ove_pm_domain_t domain);

/**
 * @brief Disable power to a peripheral domain (power-gate).
 *
 * Called when a domain's reference count transitions from 1 to 0.
 *
 * @param[in] domain  Domain identifier.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_pm_domain_disable(ove_pm_domain_t domain);

/**
 * @brief Query the time until the next scheduled RTOS event.
 *
 * @return Milliseconds until next timeout, or OVE_WAIT_FOREVER if none.
 */
uint32_t ove_hal_pm_get_next_timeout_ms(void);

/**
 * @brief Idle hook entry point — registered with the RTOS idle mechanism.
 *
 * The backend registers this function with the RTOS idle task (e.g.
 * vApplicationIdleHook on FreeRTOS, pm_notifier on Zephyr).  It calls
 * ove_pm_idle_process() in the portable layer to drive the PM state
 * machine.
 */
void ove_hal_pm_idle_hook(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_PM_H */
