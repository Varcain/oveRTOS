/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Sim LVGL backend -- replaces posix_lvgl.c when CONFIG_OVE_SIM=y.
 *
 * This
 * backend creates a memory-only LVGL display that flushes framebuffer
 * data to the sim dashboard via WebSocket.
 */

#include "ove/lvgl_internal.h"
#include "ove/sync.h"
#include "ove/time.h"
#include "ove_backend_common.h"
#include "ove/sim/ove_sim_display.h"
#include "ove/sim/ove_sim_transport.h"
#if defined(__ZEPHYR__)
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif
#include "board_desc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ove_mutex_t lvgl_mutex;
static volatile int lvgl_ready;

#ifdef CONFIG_OVE_ZERO_HEAP
static ove_mutex_storage_t lvgl_mutex_storage;
#endif

/* Framebuffer for the memory-based display driver. */
static uint8_t *fb_buf1;

/* ── High-resolution tick source ──────────────────────────────────── */

/*
 * Provide a continuous millisecond tick via ove_time_get_us() so LVGL's
 * perf monitor can measure intra-frame render/flush times.  Without this,
 * lv_tick_inc() is only called every ~33ms and all sub-frame measurements
 * return 0.
 */
static uint32_t hires_tick_cb(void)
{
	uint64_t us = 0;
	ove_time_get_us(&us);
	return (uint32_t)(us / 1000);
}

/* ── LVGL flush callback ──────────────────────────────────────────── */

static void sim_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	uint16_t x1 = (uint16_t)area->x1;
	uint16_t y1 = (uint16_t)area->y1;
	uint16_t x2 = (uint16_t)area->x2;
	uint16_t y2 = (uint16_t)area->y2;

	uint32_t w = (uint32_t)(x2 - x1 + 1);
	uint32_t h = (uint32_t)(y2 - y1 + 1);
	/* XRGB8888: 4 bytes per pixel. */
	size_t fb_len = w * h * 4;

	/* Deliver framebuffer via the transport (platform-agnostic).
	 * The transport handles delivery (WS mailbox, shared FB, or
	 * semihosting file) — no need for a separate plugin event. */
	struct ove_sim_transport *t = ove_sim_get_transport();
	ove_sim_transport_flush_display(t, px_map, fb_len, x1, y1, x2, y2);

	lv_display_flush_ready(disp);
}

/* ── Pointer input device callback (all modes) ────────────────────── */

#include "../src/ove_sim_input.h"

static void sim_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	int16_t x, y;
	uint8_t pressed;
	ove_sim_input_get(&x, &y, &pressed);
	data->point.x = x;
	data->point.y = y;
	data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ── Public LVGL API implementation ────────────────────────────────── */

int ove_lvgl_init(void)
{
	int ret;

	if (lvgl_ready)
		return OVE_OK;

#ifdef CONFIG_OVE_ZERO_HEAP
	ret = ove_mutex_init(&lvgl_mutex, &lvgl_mutex_storage);
#else
	ret = ove_mutex_create(&lvgl_mutex);
#endif
	if (ret != OVE_OK)
		return ret;

	lv_init();

	/* Register a high-resolution tick so LVGL's perf monitor can
	 * measure intra-frame render/flush times accurately, instead
	 * of relying on the 33ms-granularity lv_tick_inc() calls. */
	lv_tick_set_cb(hires_tick_cb);

	/* Create a memory-only display. */
	lv_display_t *disp = lv_display_create(OVE_DISPLAY_WIDTH, OVE_DISPLAY_HEIGHT);
	if (!disp)
		return OVE_ERR_NO_MEMORY;

	/* Allocate a single framebuffer in XRGB8888 format.
	 * Single-buffer is correct here: the sim transport writes each
	 * frame to a shared file, so there is no hardware double-buffer
	 * to overlap with.  Double-buffering would cause the dashboard
	 * to flicker between two out-of-sync frames. */
	size_t buf_size = (size_t)OVE_DISPLAY_WIDTH * OVE_DISPLAY_HEIGHT * 4;
	fb_buf1 = malloc(buf_size);
	if (!fb_buf1)
		return OVE_ERR_NO_MEMORY;

	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, fb_buf1, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(disp, sim_flush_cb);

	/* Set a theme. */
	lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
					       lv_palette_main(LV_PALETTE_RED), true,
					       &lv_font_montserrat_32);
	lv_display_set_theme(disp, th);

	/* Register mouse/touch input device.
	 * WASM: JS forwards canvas events via ccall.
	 * POSIX: dashboard sends input frames via WebSocket. */
	{
		lv_indev_t *indev = lv_indev_create();
		lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
		lv_indev_set_read_cb(indev, sim_indev_read_cb);
	}

	lvgl_ready = 1;
	return OVE_OK;
}

void ove_lvgl_lock(void)
{
	if (!lvgl_ready)
		return;
	ove_mutex_lock(lvgl_mutex, OVE_WAIT_FOREVER);
}

void ove_lvgl_unlock(void)
{
	if (!lvgl_ready)
		return;
	ove_mutex_unlock(lvgl_mutex);
}

void ove_lvgl_tick(uint32_t ms)
{
	if (!lvgl_ready)
		return;
	lv_tick_inc(ms);
}

void ove_lvgl_handler(void)
{
	if (!lvgl_ready)
		return;
	lv_timer_handler();
}
