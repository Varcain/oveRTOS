/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <time.h>
#include <errno.h>

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

static void delay_ns(uint64_t ns)
{
	struct timespec now;
	struct timespec target;

	clock_gettime(CLOCK_MONOTONIC, &now);
	target.tv_sec = now.tv_sec + (time_t)(ns / 1000000000ULL);
	target.tv_nsec = now.tv_nsec + (long)(ns % 1000000000ULL);
	if (target.tv_nsec >= 1000000000L) {
		target.tv_sec++;
		target.tv_nsec -= 1000000000L;
	}

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			       &target, NULL) == EINTR) {
		/* Restart on signal interruption */
	}
}

void ove_time_delay_ms(uint32_t ms)
{
	delay_ns((uint64_t)ms * 1000000ULL);
}

void ove_time_delay_us(uint32_t us)
{
	delay_ns((uint64_t)us * 1000ULL);
}
