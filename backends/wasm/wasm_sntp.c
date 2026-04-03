/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM SNTP — uses browser Date.now() instead of a real NTP query.
 *
 * The browser clock is already synchronized by the OS.
 * We compute the UTC offset relative to ove_time_get_us() and store it.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_NET_SNTP

#include "ove/net_sntp.h"
#include "ove/time.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <time.h>

static int64_t utc_offset_us;
static int     synced;

int ove_sntp_sync(const ove_sntp_config_t *cfg)
{
	(void)cfg;

	/* Get monotonic time. */
	uint64_t mono_us;
	ove_time_get_us(&mono_us);

	/* Get wall-clock time. */
	uint64_t wall_us;
#ifdef __EMSCRIPTEN__
	/* Date.now() returns ms since epoch. */
	double ms = EM_ASM_DOUBLE({ return Date.now(); });
	wall_us = (uint64_t)(ms * 1000.0);
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	wall_us = (uint64_t)ts.tv_sec * 1000000ULL +
		  (uint64_t)ts.tv_nsec / 1000ULL;
#endif

	utc_offset_us = (int64_t)wall_us - (int64_t)mono_us;
	synced = 1;
	return OVE_OK;
}

int ove_sntp_get_offset_us(int64_t *offset_us)
{
	if (!synced) return OVE_ERR_NOT_SUPPORTED;
	if (offset_us) *offset_us = utc_offset_us;
	return OVE_OK;
}

int ove_sntp_get_utc(uint32_t *utc_s)
{
	if (!synced) return OVE_ERR_NOT_SUPPORTED;
	uint64_t mono_us;
	ove_time_get_us(&mono_us);
	int64_t utc_us = (int64_t)mono_us + utc_offset_us;
	if (utc_s) *utc_s = (uint32_t)(utc_us / 1000000);
	return OVE_OK;
}

#endif /* CONFIG_OVE_NET_SNTP */
