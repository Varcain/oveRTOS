/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/lvgl_internal.h"
#include "ove_backend_common.h"

#ifdef CONFIG_LVGL
#include <lvgl.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

int ove_lvgl_init(void)
{
	const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display_dev)) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	display_blanking_off(display_dev);
	return OVE_OK;
}

void ove_lvgl_lock(void)
{
	lv_lock();
}

void ove_lvgl_unlock(void)
{
	lv_unlock();
}

void ove_lvgl_tick(uint32_t ms)
{
	/* Feed the app's real elapsed time as the LVGL tick.  Harmless if Zephyr's auto-init
	 * also installed an lv_tick_set_cb source (LVGL then ignores lv_tick_inc). */
	lv_tick_inc(ms);
}

void ove_lvgl_handler(void)
{
	/* Drive LVGL from the calling (app) thread.  CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE is OFF, so
	 * the app is the SOLE caller of lv_timer_handler — matching the FreeRTOS/NuttX seams.  The
	 * Zephyr workqueue path rendered but never advanced periodic lv_timers (e.g. the benchmark's
	 * scene-change timer), so scenes stuck on frame 0; driving it here is deterministic. */
	lv_timer_handler();
}
#else /* !CONFIG_LVGL */

int ove_lvgl_init(void)
{
	return OVE_OK;
}
void ove_lvgl_lock(void)
{
}
void ove_lvgl_unlock(void)
{
}
void ove_lvgl_tick(uint32_t ms)
{
	(void)ms;
}
void ove_lvgl_handler(void)
{
}

#endif /* CONFIG_LVGL */
