/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/time.h"
#include "ove_backend_common.h"
#include <zephyr/kernel.h>

int ove_time_get_us(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	*out = (uint64_t)k_cyc_to_us_floor64(k_cycle_get_32());
	return OVE_OK;
}

int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	*out = (uint64_t)k_cyc_to_ns_floor64(k_cycle_get_32());
	return OVE_OK;
}

void ove_time_delay_ms(uint32_t ms)
{
	k_sleep(K_MSEC(ms));
}

void ove_time_delay_us(uint32_t us)
{
	k_busy_wait(us);
}
