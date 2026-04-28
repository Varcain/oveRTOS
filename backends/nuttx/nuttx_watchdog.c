/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/watchdog.h"
#include "ove_backend_common.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <nuttx/timers/watchdog.h>
/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_watchdog_init(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage, uint32_t timeout_ms)
{
	if (wdt == NULL || storage == NULL || timeout_ms == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_watchdog *nw = (struct ove_watchdog *)storage;
	nw->fd = open("/dev/watchdog0", O_RDONLY);
	if (nw->fd < 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	nw->timeout_ms = timeout_ms;
	nw->started = 0;

	/* NuttX's WDIOC_SETTIMEOUT takes milliseconds on all upstream drivers
	 * we've tested (stm32 IWDG/WWDG, simulator); custom drivers that expect
	 * a different unit must be handled at the driver level, not here. */
	int ret = ioctl(nw->fd, WDIOC_SETTIMEOUT, (unsigned long)timeout_ms);
	if (ret < 0) {
		close(nw->fd);
		return OVE_ERR_NOT_SUPPORTED;
	}

	*wdt = nw;
	return OVE_OK;
}

void ove_watchdog_deinit(ove_watchdog_t wdt)
{
	if (wdt != NULL) {
		close(wdt->fd);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_WATCHDOG
int ove_watchdog_create(ove_watchdog_t *wdt, uint32_t timeout_ms)
{
	struct ove_watchdog *nw;

	if (wdt == NULL || timeout_ms == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	nw = OVE_BACKEND_MALLOC(sizeof(*nw));
	if (nw == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = ove_watchdog_init(wdt, nw, timeout_ms);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(nw);
	}
	return ret;
}

void ove_watchdog_destroy(ove_watchdog_t wdt)
{
	if (wdt != NULL) {
		ove_watchdog_deinit(wdt);
		OVE_BACKEND_FREE(wdt);
	}
}
#endif /* OVE_HEAP_WATCHDOG */

/* ─── start / stop / feed ────────────────────────────────────────────── */

int ove_watchdog_start(ove_watchdog_t wdt)
{
	int ret = ioctl(wdt->fd, WDIOC_START, 0);
	return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
	int ret = ioctl(wdt->fd, WDIOC_STOP, 0);
	return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
	int ret = ioctl(wdt->fd, WDIOC_KEEPALIVE, 0);
	return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}
