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

#include <unistd.h>

#ifdef CONFIG_PM
#include <nuttx/power/pm.h>

/* Map oveRTOS states to NuttX PM states */
static enum pm_state_e to_nuttx_state(ove_pm_state_t state)
{
	switch (state) {
	case OVE_PM_STATE_IDLE:
		return PM_IDLE;
	case OVE_PM_STATE_STANDBY:
		return PM_STANDBY;
	case OVE_PM_STATE_DEEP_SLEEP:
		return PM_SLEEP;
	default:
		return PM_NORMAL;
	}
}
#endif /* CONFIG_PM */

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
#ifdef CONFIG_PM
	enum pm_state_e nuttx_state = to_nuttx_state(state);

	(void)expected_idle_ms;

	pm_changestate(PM_IDLE_DOMAIN, nuttx_state);
	usleep(expected_idle_ms == OVE_WAIT_FOREVER ? 1000 : expected_idle_ms * 1000);
	pm_changestate(PM_IDLE_DOMAIN, PM_NORMAL);
#else
	(void)state;
	usleep(expected_idle_ms == OVE_WAIT_FOREVER ? 1000 : expected_idle_ms * 1000);
#endif
	return OVE_OK;
}

int ove_hal_pm_wake_arm(const struct ove_pm_wake_src *src)
{
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
	(void)domain;
	return OVE_OK;
}

int ove_hal_pm_domain_disable(ove_pm_domain_t domain)
{
	(void)domain;
	return OVE_OK;
}

uint32_t ove_hal_pm_get_next_timeout_ms(void)
{
	return OVE_WAIT_FOREVER;
}

void ove_hal_pm_idle_hook(void)
{
	ove_pm_idle_process();
}

#endif /* CONFIG_OVE_PM */
