/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include "posix_sleep.h"
#include <time.h>

int ove_time_get_us(uint64_t *out)
{
	struct timespec ts;
	if (!out) {
		return OVE_ERR_INVALID_PARAM;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)ts.tv_nsec / 1000ULL;
	return OVE_OK;
}

int ove_time_get_ns(uint64_t *out)
{
	struct timespec ts;
	if (!out) {
		return OVE_ERR_INVALID_PARAM;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (uint64_t)ts.tv_sec * 1000000000ULL +
	       (uint64_t)ts.tv_nsec;
	return OVE_OK;
}

void ove_time_delay_ms(uint32_t ms)
{
	posix_sleep_ms(ms);
}

void ove_time_delay_us(uint32_t us)
{
	posix_sleep_ns((uint64_t)us * 1000ULL);
}
