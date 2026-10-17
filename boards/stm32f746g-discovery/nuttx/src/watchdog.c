/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * ove_watchdog for the STM32F746G-DISCO on NuttX: the independent watchdog
 * (IWDG), driven directly because NuttX's STM32F7 port has no IWDG driver.
 * It counts the ~32 kHz LSI, as on FreeRTOS, and once started it cannot be
 * stopped. ove_reset_cause() reads the RCC reset flags.
 */

#include <stdint.h>
#include <stdlib.h>

#include "ove_config.h"
#include "ove/reset.h"
#include "ove/watchdog.h"

#define IWDG_BASE 0x40003000u
#define IWDG_KR (*(volatile uint32_t *)(IWDG_BASE + 0x00u))
#define IWDG_PR (*(volatile uint32_t *)(IWDG_BASE + 0x04u))
#define IWDG_RLR (*(volatile uint32_t *)(IWDG_BASE + 0x08u))
#define IWDG_SR (*(volatile uint32_t *)(IWDG_BASE + 0x0cu))

#define IWDG_KEY_RELOAD 0xaaaau
#define IWDG_KEY_ACCESS 0x5555u
#define IWDG_KEY_START 0xccccu
#define IWDG_PR_DIV64 4u  /* 500 Hz from the LSI: 2 ms a count */
#define IWDG_PR_DIV256 6u /* 125 Hz: 8 ms a count */
#define IWDG_RLR_MAX 4095u

#define RCC_CSR (*(volatile uint32_t *)0x40023874u)
#define RCC_CSR_RMVF (1u << 24)
#define RCC_CSR_BORRSTF (1u << 25)
#define RCC_CSR_PINRSTF (1u << 26)
#define RCC_CSR_PORRSTF (1u << 27)
#define RCC_CSR_SFTRSTF (1u << 28)
#define RCC_CSR_IWDGRSTF (1u << 29)
#define RCC_CSR_WWDGRSTF (1u << 30)
#define RCC_CSR_LPWRRSTF (1u << 31)

/* ─── reset cause ────────────────────────────────────────────────────── */

ove_reset_cause_t ove_reset_cause(void)
{
	static int latched;
	static ove_reset_cause_t cause = OVE_RESET_UNKNOWN;

	if (latched)
		return cause;
	latched = 1;

	/* Most specific first, as on FreeRTOS: a power-on also sets PIN (and BOR),
	 * and a watchdog recovery is never masked. */
	uint32_t csr = RCC_CSR;
	if (csr & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF))
		cause = OVE_RESET_WATCHDOG;
	else if (csr & RCC_CSR_SFTRSTF)
		cause = OVE_RESET_SOFTWARE;
	else if (csr & RCC_CSR_LPWRRSTF)
		cause = OVE_RESET_LOW_POWER;
	else if (csr & RCC_CSR_PORRSTF)
		cause = OVE_RESET_POWER_ON;
	else if (csr & RCC_CSR_BORRSTF)
		cause = OVE_RESET_BROWNOUT;
	else if (csr & RCC_CSR_PINRSTF)
		cause = OVE_RESET_PIN;
	RCC_CSR |= RCC_CSR_RMVF;
	return cause;
}

/* ─── watchdog ───────────────────────────────────────────────────────── */

int ove_watchdog_init(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage, uint32_t timeout_ms)
{
	if (wdt == NULL || storage == NULL || timeout_ms == 0)
		return OVE_ERR_INVALID_PARAM;

	struct ove_watchdog *w = (struct ove_watchdog *)storage;
	w->timeout_ms = timeout_ms;
	w->started = 0;
	/* Latch the reset cause before the dog can add a reset of its own. */
	(void)ove_reset_cause();
	*wdt = w;
	return OVE_OK;
}

void ove_watchdog_deinit(ove_watchdog_t wdt)
{
	(void)wdt;
}

#ifdef OVE_HEAP_WATCHDOG
int ove_watchdog_create(ove_watchdog_t *wdt, uint32_t timeout_ms)
{
	if (wdt == NULL || timeout_ms == 0)
		return OVE_ERR_INVALID_PARAM;
	struct ove_watchdog *w = calloc(1, sizeof(*w));
	if (w == NULL)
		return OVE_ERR_NO_MEMORY;
	int ret = ove_watchdog_init(wdt, (ove_watchdog_storage_t *)w, timeout_ms);
	if (ret != OVE_OK)
		free(w);
	return ret;
}

void ove_watchdog_destroy(ove_watchdog_t wdt)
{
	if (wdt != NULL) {
		ove_watchdog_deinit(wdt);
		free(wdt);
	}
}
#endif /* OVE_HEAP_WATCHDOG */

int ove_watchdog_start(ove_watchdog_t wdt)
{
	if (wdt == NULL)
		return OVE_ERR_INVALID_PARAM;

	uint32_t prescaler = IWDG_PR_DIV64;
	uint32_t reload = wdt->timeout_ms / 2u;
	if (reload > IWDG_RLR_MAX) {
		prescaler = IWDG_PR_DIV256;
		reload = wdt->timeout_ms / 8u;
	}
	if (reload > IWDG_RLR_MAX)
		reload = IWDG_RLR_MAX;
	if (reload == 0)
		reload = 1;

	IWDG_KR = IWDG_KEY_START;
	IWDG_KR = IWDG_KEY_ACCESS;
	IWDG_PR = prescaler;
	IWDG_RLR = reload;
	while (IWDG_SR != 0u) {
		/* the prescaler and reload updates cross into the LSI domain */
	}
	IWDG_KR = IWDG_KEY_RELOAD;
	wdt->started = 1;
	return OVE_OK;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
	/* The IWDG cannot be stopped once started. */
	(void)wdt;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
	if (wdt == NULL)
		return OVE_ERR_INVALID_PARAM;
	IWDG_KR = IWDG_KEY_RELOAD;
	return OVE_OK;
}
