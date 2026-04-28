/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU MPS2-AN500 LVGL display port for NuttX.
 *
 * Uses raw ARM semihosting SVC calls to write framebuffer data to
 * /dev/shm/ove-fb on the host. NuttX's C library doesn't use rdimon,
 * so we bypass it entirely with direct bkpt 0xAB calls.
 *
 * This file is compiled into the NuttX external app (not the kernel).
 */

#include <lvgl/lvgl.h>
#include <string.h>
#include <stdint.h>
#include "semihosting.h"

#define DISP_HOR_RES 480
#define DISP_VER_RES 272
#define BYTES_PER_PIXEL 2

#define FB_MAGIC 0x42465854 /* "TXFB" */
#define FB_FORMAT 0	    /* RGB565 */

struct fb_header {
	uint32_t magic;
	uint16_t width;
	uint16_t height;
	uint32_t format;
	uint32_t dirty;
};

#define DRAW_BUF_LINES 20
static uint8_t draw_buf[DISP_HOR_RES * DRAW_BUF_LINES * BYTES_PER_PIXEL]
	__attribute__((aligned(4)));

static uint16_t framebuffer[DISP_HOR_RES * DISP_VER_RES];

static int sh_fd = -1;
static int fb_dirty;

static void disp_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
	uint32_t w = area->x2 - area->x1 + 1;
	uint32_t h = area->y2 - area->y1 + 1;

	for (uint32_t y = 0; y < h; y++) {
		uint32_t fb_idx = (area->y1 + y) * DISP_HOR_RES + area->x1;
		uint32_t src_off = y * w * BYTES_PER_PIXEL;
		memcpy(&framebuffer[fb_idx], px_map + src_off, w * BYTES_PER_PIXEL);
	}

	fb_dirty = 1;

	if (lv_display_flush_is_last(display)) {
		if (sh_fd >= 0 && fb_dirty) {
			struct fb_header hdr = {
				.magic = FB_MAGIC,
				.width = DISP_HOR_RES,
				.height = DISP_VER_RES,
				.format = FB_FORMAT,
				.dirty = 1,
			};
			sh_seek(sh_fd, 0);
			sh_write(sh_fd, &hdr, sizeof(hdr));
			sh_write(sh_fd, framebuffer, sizeof(framebuffer));
			fb_dirty = 0;
		}
	}

	lv_display_flush_ready(display);
}

void lv_port_disp_qemu_init(void)
{
	/* Open shmem file via semihosting. mode 7 = "r+b" */
	sh_fd = sh_open("/dev/shm/ove-fb", 7);
	/* sh_fd < 0 means no display (--headless) — that's fine */

	memset(framebuffer, 0, sizeof(framebuffer));

	lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, disp_flush_cb);
}
