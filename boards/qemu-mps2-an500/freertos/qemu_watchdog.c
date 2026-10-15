/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Watchdog for QEMU MPS2-AN500 on FreeRTOS: not supported. The FreeRTOS
 * watchdog backend drives the STM32 IWDG, which this machine does not have.
 */

#include "ove/watchdog.h"

int ove_watchdog_init(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage, uint32_t timeout_ms)
{
	(void)wdt;
	(void)storage;
	(void)timeout_ms;
	return OVE_ERR_NOT_SUPPORTED;
}

void ove_watchdog_deinit(ove_watchdog_t wdt)
{
	(void)wdt;
}

#ifdef OVE_HEAP_WATCHDOG
int ove_watchdog_create(ove_watchdog_t *wdt, uint32_t timeout_ms)
{
	(void)wdt;
	(void)timeout_ms;
	return OVE_ERR_NOT_SUPPORTED;
}

void ove_watchdog_destroy(ove_watchdog_t wdt)
{
	(void)wdt;
}
#endif /* OVE_HEAP_WATCHDOG */

int ove_watchdog_start(ove_watchdog_t wdt)
{
	(void)wdt;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
	(void)wdt;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
	(void)wdt;
	return OVE_ERR_NOT_SUPPORTED;
}
