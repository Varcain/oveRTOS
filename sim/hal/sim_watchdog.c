/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Simulated watchdog — software timeout with logging.
 *
 * A background pthread checks all started watchdogs periodically.
 * If a watchdog hasn't been fed within its timeout, a warning is
 * logged (visible in the dashboard event log).
 *
 * Replaces the POSIX no-op watchdog when CONFIG_OVE_SIM=y.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

/* ── Watchdog registry ─────────────────────────────────────────────── */

#define MAX_WATCHDOGS 8

static struct ove_watchdog *wdt_list[MAX_WATCHDOGS];
static int wdt_count;
static pthread_mutex_t wdt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t wdt_thread;
static volatile int wdt_running;

static uint64_t wdt_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)ts.tv_nsec / 1000ULL;
}

static void *watchdog_monitor(void *arg)
{
	(void)arg;
	while (wdt_running) {
		usleep(200000); /* check every 200ms */

		uint64_t now = wdt_now_us();
		pthread_mutex_lock(&wdt_lock);
		for (int i = 0; i < wdt_count; i++) {
			struct ove_watchdog *w = wdt_list[i];
			if (!w || !w->started)
				continue;
			uint64_t elapsed_ms =
				(now - w->last_feed_us) / 1000;
			if (elapsed_ms > w->timeout_ms) {
				fprintf(stderr,
					"[watchdog] TIMEOUT! Not fed for "
					"%llu ms (limit %u ms)\n",
					(unsigned long long)elapsed_ms,
					w->timeout_ms);
				/* Reset the timer so we don't spam. */
				w->last_feed_us = now;
			}
		}
		pthread_mutex_unlock(&wdt_lock);
	}
	return NULL;
}

static void ensure_monitor(void)
{
	if (wdt_running) return;
	wdt_running = 1;
	pthread_create(&wdt_thread, NULL, watchdog_monitor, NULL);
}

/* ── Public API ────────────────────────────────────────────────────── */

int ove_watchdog_init(ove_watchdog_t *wdt,
		      ove_watchdog_storage_t *storage,
		      uint32_t timeout_ms)
{
	if (!wdt || !storage) return OVE_ERR_INVALID_PARAM;
	struct ove_watchdog *w = (struct ove_watchdog *)storage;
	memset(w, 0, sizeof(*w));
	w->timeout_ms = timeout_ms;
	w->last_feed_us = wdt_now_us();
	*wdt = w;
	return OVE_OK;
}

void ove_watchdog_deinit(ove_watchdog_t wdt)
{
	if (!wdt) return;
	pthread_mutex_lock(&wdt_lock);
	for (int i = 0; i < wdt_count; i++) {
		if (wdt_list[i] == wdt) {
			wdt_list[i] = wdt_list[--wdt_count];
			break;
		}
	}
	pthread_mutex_unlock(&wdt_lock);
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_watchdog_create(ove_watchdog_t *wdt, uint32_t timeout_ms)
{
	struct ove_watchdog *w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (!w) return OVE_ERR_NO_MEMORY;
	memset(w, 0, sizeof(*w));
	w->timeout_ms = timeout_ms;
	w->last_feed_us = wdt_now_us();
	*wdt = w;
	return OVE_OK;
}
#endif

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_watchdog_destroy(ove_watchdog_t wdt)
{
	ove_watchdog_deinit(wdt);
	if (wdt) OVE_BACKEND_FREE(wdt);
}
#endif

int ove_watchdog_start(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) return OVE_ERR_INVALID_PARAM;

	w->last_feed_us = wdt_now_us();
	w->started = 1;

	ensure_monitor();

	pthread_mutex_lock(&wdt_lock);
	if (wdt_count < MAX_WATCHDOGS)
		wdt_list[wdt_count++] = w;
	pthread_mutex_unlock(&wdt_lock);

	return OVE_OK;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) return OVE_ERR_INVALID_PARAM;
	w->started = 0;
	return OVE_OK;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
	struct ove_watchdog *w = wdt;
	if (!w) return OVE_ERR_INVALID_PARAM;
	w->last_feed_us = wdt_now_us();
	return OVE_OK;
}
