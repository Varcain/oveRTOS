/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stub watchdog backend for bare-metal testing.
 * Same as posix_watchdog — uses only stdlib.
 */

#include "ove/ove.h"
#include <stdlib.h>

int ove_watchdog_init(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage,
		      uint32_t timeout_ms)
{
	if (!wdt || !storage) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_watchdog *w = (struct ove_watchdog *)storage;
	w->timeout_ms = timeout_ms;
	w->started = 0;
	*wdt = w;
	return OVE_OK;
}

void ove_watchdog_deinit(ove_watchdog_t wdt)
{
	(void)wdt;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_watchdog_create(ove_watchdog_t *wdt, uint32_t timeout_ms)
{
	struct ove_watchdog *w = calloc(1, sizeof(*w));
	if (!w) {
		return OVE_ERR_NO_MEMORY;
	}
	w->timeout_ms = timeout_ms;
	*wdt = w;
	return OVE_OK;
}

void ove_watchdog_destroy(ove_watchdog_t wdt)
{
	free(wdt);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_watchdog_start(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) {
		return OVE_ERR_INVALID_PARAM;
	}
	w->started = 1;
	return OVE_OK;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) {
		return OVE_ERR_INVALID_PARAM;
	}
	w->started = 0;
	return OVE_OK;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}
