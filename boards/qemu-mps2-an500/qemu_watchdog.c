/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Watchdog for QEMU MPS2-AN500, shared by FreeRTOS and NuttX: the CMSDK APB
 * watchdog at 0x40008000, clocked from the system clock. Its counter raises an
 * interrupt (wired to NMI on this machine) the first time it reaches zero
 * and resets the machine the second time, so it is loaded with half the
 * timeout: an unfed dog resets the board timeout_ms after the last feed.
 * Unlike the STM32 IWDG it can be stopped again.
 */

#include "ove_config.h"
#include "ove/reset.h"
#include "ove/watchdog.h"
#include "board_desc.h"
#include <stdint.h>
#include <stdlib.h>
#if defined(CONFIG_OVE_RTOS_NUTTX)
#include <nuttx/irq.h>
/* NVIC_IRQ_NMI in NuttX's arch-private arch/arm/src/armv7-m/nvic.h */
#define WDOG_NMI_IRQ 2
#endif

#define WDOG_BASE 0x40008000u
#define WDOG_REG(off) (*(volatile uint32_t *)(uintptr_t)(WDOG_BASE + (off)))
#define WDOG_LOAD WDOG_REG(0x000u)
#define WDOG_CONTROL WDOG_REG(0x008u)
#define WDOG_INTCLR WDOG_REG(0x00cu)
#define WDOG_LOCK WDOG_REG(0xc00u)

#define WDOG_CONTROL_INTEN (1u << 0) /* also runs the counter */
#define WDOG_CONTROL_RESEN (1u << 1)
#define WDOG_UNLOCK 0x1ACCE551u

/* The counter runs from WDOGCLK, the system clock. */
static uint32_t half_timeout_ticks(uint32_t timeout_ms)
{
	uint64_t ticks = (uint64_t)OVE_CLOCK_SYSCLK_MHZ * 1000u * timeout_ms / 2u;
	return ticks > UINT32_MAX ? UINT32_MAX : (ticks ? (uint32_t)ticks : 1u);
}

/* Control writes are ignored while the lock is closed. */
static void wdog_write(volatile uint32_t *reg, uint32_t value)
{
	WDOG_LOCK = WDOG_UNLOCK;
	*reg = value;
	WDOG_LOCK = 0;
}

/* The first expiry's NMI only announces the reset to come: returning leaves
 * the count running, and feeding before the second expiry still saves the
 * board. FreeRTOS takes the NMI vector by name; NuttX dispatches it as an IRQ
 * and panics on one nobody attached. */
#if defined(CONFIG_OVE_RTOS_NUTTX)
static int wdog_nmi(int irq, void *context, void *arg)
{
	(void)irq;
	(void)context;
	(void)arg;
	return 0;
}
#else
void NMI_Handler(void)
{
}
#endif

/* The CMSDK watchdog keeps no record of why the machine last reset. */
ove_reset_cause_t ove_reset_cause(void)
{
	return OVE_RESET_UNKNOWN;
}

int ove_watchdog_init(ove_watchdog_t *wdt, ove_watchdog_storage_t *storage, uint32_t timeout_ms)
{
	if (!wdt || !storage || timeout_ms == 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_watchdog *w = (struct ove_watchdog *)storage;
	w->timeout_ms = timeout_ms;
	w->started = 0;
#if defined(CONFIG_OVE_RTOS_NUTTX)
	irq_attach(WDOG_NMI_IRQ, wdog_nmi, NULL);
#endif
	*wdt = w;
	return OVE_OK;
}

void ove_watchdog_deinit(ove_watchdog_t wdt)
{
	(void)ove_watchdog_stop(wdt);
}

#ifdef OVE_HEAP_WATCHDOG
int ove_watchdog_create(ove_watchdog_t *wdt, uint32_t timeout_ms)
{
	if (!wdt || timeout_ms == 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_watchdog *w = calloc(1, sizeof(*w));
	if (!w) {
		return OVE_ERR_NO_MEMORY;
	}
	return ove_watchdog_init(wdt, (ove_watchdog_storage_t *)w, timeout_ms);
}

void ove_watchdog_destroy(ove_watchdog_t wdt)
{
	if (wdt) {
		ove_watchdog_deinit(wdt);
		free(wdt);
	}
}
#endif /* OVE_HEAP_WATCHDOG */

int ove_watchdog_start(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* LOAD reloads the count; turning INTEN on starts it. */
	wdog_write(&WDOG_LOAD, half_timeout_ticks(w->timeout_ms));
	wdog_write(&WDOG_INTCLR, 1u);
	wdog_write(&WDOG_CONTROL, WDOG_CONTROL_INTEN | WDOG_CONTROL_RESEN);
	w->started = 1;
	return OVE_OK;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (w->started) {
		wdog_write(&WDOG_CONTROL, 0);
		wdog_write(&WDOG_INTCLR, 1u);
		w->started = 0;
	}
	return OVE_OK;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* Clearing the interrupt also reloads the count. */
	wdog_write(&WDOG_INTCLR, 1u);
	return OVE_OK;
}
