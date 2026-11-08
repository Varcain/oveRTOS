/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <nuttx/clock.h>
#include <stdint.h>
#define WDOG_MAX_DELAY (CLOCK_MAX >> 2)
typedef uintptr_t wdparm_t;
typedef void (*wdentry_t)(wdparm_t);
struct wdog_s {
	clock_t delay;
	wdentry_t handler;
	wdparm_t arg;
	unsigned active;
};
int wd_start(struct wdog_s *, clock_t, wdentry_t, wdparm_t);
int wd_cancel(struct wdog_s *);
