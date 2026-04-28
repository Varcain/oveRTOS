/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/watchdog.h"
#include "ove_backend_common.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/device.h>
/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_watchdog_init(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage, uint32_t timeout_ms)
{
	struct wdt_timeout_cfg cfg;
	const struct device *dev;

	if (wdt == NULL || storage == NULL || timeout_ms == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
	if (dev == NULL || !device_is_ready(dev)) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	struct ove_watchdog *zw = (struct ove_watchdog *)storage;
	zw->dev = dev;
	zw->timeout_ms = timeout_ms;
	zw->started = 0;

	cfg.window.min = 0;
	cfg.window.max = timeout_ms;
	cfg.callback = NULL;
	cfg.flags = WDT_FLAG_RESET_SOC;

	zw->channel_id = wdt_install_timeout(dev, &cfg);
	if (zw->channel_id < 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	*wdt = zw;
	return OVE_OK;
}

void ove_watchdog_deinit(ove_watchdog_t wdt)
{
	(void)wdt;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_WATCHDOG
int ove_watchdog_create(ove_watchdog_t *wdt, uint32_t timeout_ms)
{
	if (wdt == NULL || timeout_ms == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_watchdog *zw = OVE_BACKEND_MALLOC(sizeof(*zw));
	if (zw == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = ove_watchdog_init(wdt, zw, timeout_ms);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(zw);
	}
	return ret;
}

void ove_watchdog_destroy(ove_watchdog_t wdt)
{
	OVE_BACKEND_FREE(wdt);
}
#endif /* OVE_HEAP_WATCHDOG */

/* ─── start / stop / feed ────────────────────────────────────────────── */

int ove_watchdog_start(ove_watchdog_t wdt)
{
	int ret = wdt_setup(wdt->dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
	int ret = wdt_disable(wdt->dev);
	return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
	int ret = wdt_feed(wdt->dev, wdt->channel_id);
	return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}
