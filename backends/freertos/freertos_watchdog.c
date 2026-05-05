/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* struct ove_watchdog is defined in ove_storage_freertos.h (pulled in via
 * ove/storage.h) so every consumer — C app, C++ binding, Rust/Zig storage
 * size probes — sees the same 20-byte layout this backend writes. */
#include "ove/watchdog.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
static void compute_prescaler_reload(uint32_t timeout_ms, uint32_t *prescaler, uint32_t *reload)
{
	/* LSI ~32 kHz.
	 * PR=4 (IWDG_PRESCALER_64): reload = timeout_ms / 2
	 * PR=6 (IWDG_PRESCALER_256): reload = timeout_ms / 8
	 */
	*prescaler = IWDG_PRESCALER_64;
	*reload = timeout_ms / 2;
	if (*reload > 4095) {
		*prescaler = IWDG_PRESCALER_256;
		*reload = timeout_ms / 8;
	}
	if (*reload > 4095) {
		*reload = 4095;
	}
	if (*reload == 0) {
		*reload = 1;
	}
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_watchdog_init(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage, uint32_t timeout_ms)
{
	uint32_t prescaler, reload;

	if (wdt == NULL || storage == NULL || timeout_ms == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_watchdog *fw = (struct ove_watchdog *)storage;
	fw->timeout_ms = timeout_ms;
	compute_prescaler_reload(timeout_ms, &prescaler, &reload);

	fw->hiwdg.Instance = IWDG;
	fw->hiwdg.Init.Prescaler = prescaler;
	fw->hiwdg.Init.Reload = reload;
	fw->hiwdg.Init.Window = IWDG_WINDOW_DISABLE;

	*wdt = fw;
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
	struct ove_watchdog *fw;

	if (wdt == NULL || timeout_ms == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	fw = OVE_BACKEND_MALLOC(sizeof(*fw));
	if (fw == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = ove_watchdog_init(wdt, (ove_watchdog_storage_t *)fw, timeout_ms);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(fw);
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

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_watchdog_start(ove_watchdog_t wdt)
{
	if (wdt == NULL) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (HAL_IWDG_Init(&wdt->hiwdg) != HAL_OK) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
	/* IWDG cannot be stopped once started on STM32 */
	(void)wdt;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
	if (wdt == NULL) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (HAL_IWDG_Refresh(&wdt->hiwdg) != HAL_OK) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}
