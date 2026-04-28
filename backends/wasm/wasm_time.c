/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM/Emscripten time backend.
 *
 * clock_gettime(CLOCK_MONOTONIC) works in Emscripten.
 * clock_nanosleep does NOT — replaced with usleep().
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <time.h>
#include <unistd.h>

int ove_time_get_us(uint64_t *out)
{
	struct timespec ts;
	if (!out)
		return OVE_ERR_INVALID_PARAM;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
	return OVE_OK;
}

int ove_time_get_ns(uint64_t *out)
{
	struct timespec ts;
	if (!out)
		return OVE_ERR_INVALID_PARAM;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	return OVE_OK;
}

void ove_time_delay_ms(uint32_t ms)
{
	usleep((useconds_t)ms * 1000);
}

void ove_time_delay_us(uint32_t us)
{
	usleep((useconds_t)us);
}
