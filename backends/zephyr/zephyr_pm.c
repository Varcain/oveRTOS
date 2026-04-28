/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PM

#include "ove/hal/hal_pm.h"
#include "ove/log.h"
#include "ove_backend_common.h"

#include <zephyr/kernel.h>

#ifdef CONFIG_PM
#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#endif

/* Map oveRTOS states to Zephyr PM states */
#ifdef CONFIG_PM
static enum pm_state to_zephyr_state(ove_pm_state_t state)
{
	switch (state) {
	case OVE_PM_STATE_IDLE:
		return PM_STATE_RUNTIME_IDLE;
	case OVE_PM_STATE_STANDBY:
		return PM_STATE_SUSPEND_TO_IDLE;
	case OVE_PM_STATE_DEEP_SLEEP:
		return PM_STATE_STANDBY;
	default:
		return PM_STATE_ACTIVE;
	}
}
#endif

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
	(void)expected_idle_ms;

#ifdef CONFIG_PM
	struct pm_state_info info = {
		.state = to_zephyr_state(state),
		.substate_id = 0,
		.min_residency_us = 0,
		.exit_latency_us = 0,
	};
	pm_state_force(0, &info);
	/* The Zephyr kernel enters the forced state on next idle.
	 * k_sleep triggers the idle path.
	 */
	if (expected_idle_ms == OVE_WAIT_FOREVER)
		k_sleep(K_FOREVER);
	else
		k_msleep(expected_idle_ms);
#else
	k_msleep(expected_idle_ms == OVE_WAIT_FOREVER ? 1 : (int32_t)expected_idle_ms);
#endif
	return OVE_OK;
}

int ove_hal_pm_wake_arm(const struct ove_pm_wake_src *src)
{
	/* GPIO wake: Zephyr GPIO interrupts configured via ove_gpio layer.
	 * Timer/UART/RTC: handled by Zephyr device drivers natively.
	 */
	(void)src;
	return OVE_OK;
}

int ove_hal_pm_wake_disarm(const struct ove_pm_wake_src *src)
{
	(void)src;
	return OVE_OK;
}

int ove_hal_pm_domain_enable(ove_pm_domain_t domain)
{
#ifdef CONFIG_PM_DEVICE
	/* Board-specific: call pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME)
	 * for devices in this domain.  Requires a board-level domain→device map.
	 */
	(void)domain;
#else
	(void)domain;
#endif
	return OVE_OK;
}

int ove_hal_pm_domain_disable(ove_pm_domain_t domain)
{
#ifdef CONFIG_PM_DEVICE
	(void)domain;
#else
	(void)domain;
#endif
	return OVE_OK;
}

uint32_t ove_hal_pm_get_next_timeout_ms(void)
{
	/* Return OVE_WAIT_FOREVER — Zephyr's kernel idle path handles
	 * the actual next-timeout calculation internally. */
	return OVE_WAIT_FOREVER;
}

void ove_hal_pm_idle_hook(void)
{
	ove_pm_idle_process();
}

#endif /* CONFIG_OVE_PM */
