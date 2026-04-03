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
 * Instead of creating an SDL2 window via lv_sdl_window_create(), this
 * backend creates a memory-only LVGL display that flushes framebuffer
 * data to the sim dashboard via WebSocket.
 */

#include "ove/lvgl_internal.h"
#include "ove/sync.h"
#include "ove_backend_common.h"
#include "ove/sim/ove_sim_display.h"
#include "../src/ove_sim_ws.h"
#include "lvgl.h"
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
static uint8_t *fb_buf2;

/* ── LVGL flush callback ──────────────────────────────────────────── */

static void sim_flush_cb(lv_display_t *disp, const lv_area_t *area,
			 uint8_t *px_map)
{
	uint16_t x1 = (uint16_t)area->x1;
	uint16_t y1 = (uint16_t)area->y1;
	uint16_t x2 = (uint16_t)area->x2;
	uint16_t y2 = (uint16_t)area->y2;

	uint32_t w = (uint32_t)(x2 - x1 + 1);
	uint32_t h = (uint32_t)(y2 - y1 + 1);
	/* XRGB8888: 4 bytes per pixel. */
	size_t fb_len = w * h * 4;

#ifdef __EMSCRIPTEN__
	/* WASM: write directly to shared framebuffer in WASM heap.
	 * JS polls it via requestAnimationFrame. */
	extern void ove_sim_wasm_fb_write(const uint8_t *pixels,
					  uint32_t size,
					  uint16_t width, uint16_t height);
	ove_sim_wasm_fb_write(px_map, (uint32_t)fb_len, w, h);
#else
	/* POSIX: send via WS mailbox. */
	if (ove_sim_ws_has_clients()) {
		size_t hdr_len = 8;
		size_t total = hdr_len + fb_len;
		uint8_t *frame = malloc(total);
		if (frame) {
			uint16_t coords[4] = {x1, y1, x2, y2};
			memcpy(frame, coords, 8);
			memcpy(frame + 8, px_map, fb_len);
			ove_sim_ws_broadcast(OVE_SIM_WS_FRAME_FB,
					     frame, total);
			free(frame);
		}
	}
	/* Also emit via transport (for QEMU mode). */
	ove_sim_display_flush(px_map, fb_len, x1, y1, x2, y2);
#endif

	lv_display_flush_ready(disp);
}

/* ── WASM input device callback ────────────────────────────────────── */

#ifdef __EMSCRIPTEN__
#include "../src/ove_sim_wasm_input.h"

static void sim_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	data->point.x = ove_wasm_input.x;
	data->point.y = ove_wasm_input.y;
	data->state = ove_wasm_input.pressed
		? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
#endif

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

	/* Create a memory-only display (no SDL2). */
	lv_display_t *disp = lv_display_create(OVE_DISPLAY_WIDTH,
					       OVE_DISPLAY_HEIGHT);
	if (!disp)
		return OVE_ERR_NO_MEMORY;

	/* Allocate double-buffered framebuffers.
	 * Use native XRGB8888 to match LV_COLOR_DEPTH=32 (avoids
	 * format conversion and ensures pixel data is correct). */
	size_t buf_size = (size_t)OVE_DISPLAY_WIDTH * OVE_DISPLAY_HEIGHT * 4;
	fb_buf1 = malloc(buf_size);
	fb_buf2 = malloc(buf_size);
	if (!fb_buf1 || !fb_buf2)
		return OVE_ERR_NO_MEMORY;

	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, fb_buf1, fb_buf2, buf_size,
			       LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(disp, sim_flush_cb);

	/* Set a theme. */
	lv_theme_t *th = lv_theme_default_init(
		disp,
		lv_palette_main(LV_PALETTE_BLUE),
		lv_palette_main(LV_PALETTE_RED),
		true, &lv_font_montserrat_32);
	lv_display_set_theme(disp, th);

#ifdef __EMSCRIPTEN__
	/* Register mouse/touch input device for WASM.
	 * JS forwards canvas events to the shared input struct. */
	{
		lv_indev_t *indev = lv_indev_create();
		lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
		lv_indev_set_read_cb(indev, sim_indev_read_cb);
	}
#endif

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
