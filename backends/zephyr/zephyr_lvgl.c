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
	/* Zephyr LVGL integration handles tick automatically */
	(void)ms;
}

void ove_lvgl_handler(void)
{
	/* Zephyr LVGL integration handles rendering via workqueue */
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
