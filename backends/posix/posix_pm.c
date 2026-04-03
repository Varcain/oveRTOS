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

static const char *state_name(ove_pm_state_t state)
{
	switch (state) {
	case OVE_PM_STATE_ACTIVE:     return "ACTIVE";
	case OVE_PM_STATE_IDLE:       return "IDLE";
	case OVE_PM_STATE_STANDBY:    return "STANDBY";
	case OVE_PM_STATE_DEEP_SLEEP: return "DEEP_SLEEP";
	default:                      return "UNKNOWN";
	}
}

static const char *wake_type_name(ove_pm_wake_type_t type)
{
	switch (type) {
	case OVE_PM_WAKE_GPIO:  return "GPIO";
	case OVE_PM_WAKE_TIMER: return "TIMER";
	case OVE_PM_WAKE_UART:  return "UART";
	case OVE_PM_WAKE_RTC:   return "RTC";
	default:                return "UNKNOWN";
	}
}

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
	OVE_LOG_INF("pm: [POSIX] enter %s (expected %u ms)",
		    state_name(state), expected_idle_ms);
	/* Brief sleep to simulate state transition for test observability */
	usleep(1000);
	return OVE_OK;
}

int ove_hal_pm_wake_arm(const struct ove_pm_wake_src *src)
{
	OVE_LOG_DBG("pm: [POSIX] arm wake %s", wake_type_name(src->type));
	return OVE_OK;
}

int ove_hal_pm_wake_disarm(const struct ove_pm_wake_src *src)
{
	OVE_LOG_DBG("pm: [POSIX] disarm wake %s", wake_type_name(src->type));
	return OVE_OK;
}

int ove_hal_pm_domain_enable(ove_pm_domain_t domain)
{
	OVE_LOG_DBG("pm: [POSIX] domain %d enabled", domain);
	return OVE_OK;
}

int ove_hal_pm_domain_disable(ove_pm_domain_t domain)
{
	OVE_LOG_DBG("pm: [POSIX] domain %d disabled", domain);
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
