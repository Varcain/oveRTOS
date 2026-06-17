/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/lvgl_internal.h"
#include "ove/sync.h"
#include "ove_backend_common.h"
#include "lvgl.h"
#include "lv_port_disp.h"

static ove_mutex_t lvgl_mutex;
static int lvgl_initialized;

#ifdef CONFIG_OVE_ZERO_HEAP
static ove_mutex_storage_t lvgl_mutex_storage;
#endif

int ove_lvgl_init(void)
{
	int ret;

	if (__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE)) {
		return OVE_OK;
	}

#ifdef CONFIG_OVE_ZERO_HEAP
	ret = ove_mutex_init(&lvgl_mutex, &lvgl_mutex_storage);
#else
	ret = ove_mutex_create(&lvgl_mutex);
#endif
	if (ret != OVE_OK) {
		return ret;
	}

	lv_init();
	lv_port_disp_init();

	lv_theme_t *th = lv_theme_default_init(lv_display_get_default(),
					       lv_palette_main(LV_PALETTE_BLUE),
					       lv_palette_main(LV_PALETTE_RED), true,
					       &lv_font_montserrat_32);
	lv_display_set_theme(lv_display_get_default(), th);

	/*
	 * Publish readiness only after LVGL is fully initialised (release).
	 * The lock/tick/handler entry points no-op until this flag is set, so
	 * an application thread that calls into LVGL before ove_lvgl_init()
	 * completes — e.g. a graphics worker spawned ahead of init — does
	 * nothing instead of touching an uninitialised mutex or LVGL state.
	 */
	__atomic_store_n(&lvgl_initialized, 1, __ATOMIC_RELEASE);

	return OVE_OK;
}

void ove_lvgl_lock(void)
{
	if (!__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE))
		return;
	ove_mutex_lock(lvgl_mutex, OVE_WAIT_FOREVER);
}

void ove_lvgl_unlock(void)
{
	if (!__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE))
		return;
	ove_mutex_unlock(lvgl_mutex);
}

void ove_lvgl_tick(uint32_t ms)
{
	if (!__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE))
		return;
	lv_tick_inc(ms);
}

void ove_lvgl_handler(void)
{
	if (!__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE))
		return;
	lv_timer_handler();
}
