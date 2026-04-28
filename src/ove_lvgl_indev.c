/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared keypad / encoder input device registration for LVGL.
 *
 * Users call ove_lvgl_register_keypad() / ove_lvgl_register_encoder()
 * with a read callback that supplies the current input state. The
 * functions below create a single lv_indev_t per type on first call
 * and install an internal shim that dispatches to the user callback
 * every LVGL refresh cycle.
 *
 * All four RTOS backends link this file via their respective
 * CMakeLists / Makefile — the LVGL indev API is platform-agnostic,
 * so one implementation serves every target.
 */

#include "ove/lvgl_internal.h"

#ifdef CONFIG_OVE_LVGL

#if defined(__ZEPHYR__)
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif

#include <stddef.h>

/* ── State ─────────────────────────────────────────────────────────── */

static lv_indev_t *g_keypad_indev;
static lv_indev_t *g_encoder_indev;
static ove_lvgl_keypad_read_fn_t g_keypad_read;
static ove_lvgl_encoder_read_fn_t g_encoder_read;

/* ── LVGL read shims ───────────────────────────────────────────────── */

static void ove_keypad_shim(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	data->key = 0;
	data->state = LV_INDEV_STATE_RELEASED;

	if (g_keypad_read) {
		uint32_t key = 0;
		bool pressed = false;
		if (g_keypad_read(&key, &pressed)) {
			data->key = key;
			data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
		}
	}
}

static void ove_encoder_shim(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	data->enc_diff = 0;
	data->state = LV_INDEV_STATE_RELEASED;

	if (g_encoder_read) {
		int16_t diff = 0;
		bool pressed = false;
		if (g_encoder_read(&diff, &pressed)) {
			data->enc_diff = diff;
			data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
		}
	}
}

/* ── Public API ────────────────────────────────────────────────────── */

int ove_lvgl_register_keypad(ove_lvgl_keypad_read_fn_t cb)
{
	g_keypad_read = cb;
	if (!g_keypad_indev) {
		g_keypad_indev = lv_indev_create();
		if (!g_keypad_indev) {
			return OVE_ERR_NO_MEMORY;
		}
		lv_indev_set_type(g_keypad_indev, LV_INDEV_TYPE_KEYPAD);
		lv_indev_set_read_cb(g_keypad_indev, ove_keypad_shim);
	}
	return OVE_OK;
}

int ove_lvgl_register_encoder(ove_lvgl_encoder_read_fn_t cb)
{
	g_encoder_read = cb;
	if (!g_encoder_indev) {
		g_encoder_indev = lv_indev_create();
		if (!g_encoder_indev) {
			return OVE_ERR_NO_MEMORY;
		}
		lv_indev_set_type(g_encoder_indev, LV_INDEV_TYPE_ENCODER);
		lv_indev_set_read_cb(g_encoder_indev, ove_encoder_shim);
	}
	return OVE_OK;
}

void *ove_lvgl_get_keypad_indev(void)
{
	return g_keypad_indev;
}

void *ove_lvgl_get_encoder_indev(void)
{
	return g_encoder_indev;
}

#endif /* CONFIG_OVE_LVGL */
