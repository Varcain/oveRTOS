/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
/* This inert SDK profile models the native 32-bit watchdog boundary. */
#define WDOG_MAX_DELAY (UINT32_MAX >> 2)
struct wdog_s {
	unsigned opaque;
};
