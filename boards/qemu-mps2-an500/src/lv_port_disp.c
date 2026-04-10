/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU MPS2-AN500 LVGL display port.
 *
 * Flushes rendered pixels to /dev/shm/ove-fb via semihosting file I/O.
 * The host viewer (ove-dashboard-bridge.py) reads and displays them.
 */

#include "lvgl.h"
#include "board_desc.h"
#include <stdio.h>
#include <string.h>

#define DISP_HOR_RES OVE_DISPLAY_WIDTH
#define DISP_VER_RES OVE_DISPLAY_HEIGHT
#define BYTES_PER_PIXEL 2

/* Framebuffer header for viewer protocol */
#define FB_MAGIC   0x42465854  /* "TXFB" */
#define FB_FORMAT  0           /* RGB565 */

struct fb_header {
	uint32_t magic;
	uint16_t width;
	uint16_t height;
	uint32_t format;
	uint32_t dirty;
};

/* Draw buffer — 20 lines worth of RGB565 */
#define DRAW_BUF_LINES 20
static uint8_t draw_buf[DISP_HOR_RES * DRAW_BUF_LINES * BYTES_PER_PIXEL]
    __attribute__((aligned(4)));

/* Full framebuffer in RAM — LVGL flushes partial strips here,
 * then we write the whole thing to shmem periodically */
static uint16_t framebuffer[DISP_HOR_RES * DISP_VER_RES];

static FILE *fb_file;
static int fb_dirty;

static void disp_flush_cb(lv_display_t *display, const lv_area_t *area,
			   uint8_t *px_map)
{
	uint32_t w = area->x2 - area->x1 + 1;
	uint32_t h = area->y2 - area->y1 + 1;

	/* Copy rendered strip into our full framebuffer */
	for (uint32_t y = 0; y < h; y++) {
		uint32_t fb_idx = (area->y1 + y) * DISP_HOR_RES + area->x1;
		uint32_t src_off = y * w * BYTES_PER_PIXEL;
		memcpy(&framebuffer[fb_idx], px_map + src_off,
		       w * BYTES_PER_PIXEL);
	}

	fb_dirty = 1;

	/* If this is the last flush for this refresh cycle, write to shmem */
	if (lv_display_flush_is_last(display)) {
		if (fb_file && fb_dirty) {
			struct fb_header hdr = {
				.magic  = FB_MAGIC,
				.width  = DISP_HOR_RES,
				.height = DISP_VER_RES,
				.format = FB_FORMAT,
				.dirty  = 1,
			};
			fseek(fb_file, 0, SEEK_SET);
			fwrite(&hdr, 1, sizeof(hdr), fb_file);
			fwrite(framebuffer, 1, sizeof(framebuffer), fb_file);
			fflush(fb_file);
			fb_dirty = 0;
		}
	}

	lv_display_flush_ready(display);
}

void lv_port_disp_init(void)
{
	/* Open shmem file via semihosting (pre-created by qemu-run.sh --display) */
	fb_file = fopen("/dev/shm/ove-fb", "r+b");
	/* NULL is fine — means no --display flag, run headless */

	memset(framebuffer, 0, sizeof(framebuffer));

	lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, disp_flush_cb);
}
