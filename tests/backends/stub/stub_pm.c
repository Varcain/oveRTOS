/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stub PM HAL for testing.
 */

#include "ove/hal/hal_pm.h"
#include "ove/types.h"

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
	(void)state;
	(void)expected_idle_ms;
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
}
