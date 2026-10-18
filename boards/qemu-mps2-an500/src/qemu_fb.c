/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * QEMU MPS2-AN500 framebuffer backend for the Linux personality (/dev/fb0).
 * The panel is a RAM RGB565 buffer pushed to /dev/shm/ove-fb over semihosting
 * file I/O (the same TXFB header protocol the native lv_port_disp.c uses), read
 * by the host viewer (config/scripts/ove-dashboard-bridge.py). Each present is
 * one direct semihosting write: stdio would push the frame through a small
 * buffer, one trap per chunk, and at the fb tick rate that starves the guest.
 * Without the shm file (no viewer has created it) present() no-ops.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FB)

#include "ove/hal/hal_fb.h"
#include "ove/types.h"
#include "board_desc.h"

#include <stdint.h>
#include <string.h>

#include "semihosting.h"

#define FB_W OVE_DISPLAY_WIDTH
#define FB_H OVE_DISPLAY_HEIGHT
#ifndef OVE_DISPLAY_FORMAT_RGB565
#error "this framebuffer backend drives RGB565 only (board.yaml display.color_format)"
#endif
#define FB_MAGIC 0x42465854u /* "TXFB" */
#define FB_FORMAT_RGB565 0

/* Viewer protocol header (matches boards/.../src/lv_port_disp.c). */
struct fb_header {
	uint32_t magic;
	uint16_t width;
	uint16_t height;
	uint32_t format;
	uint32_t dirty;
};

static uint16_t g_fb[FB_W * FB_H];
static int g_shm = -1;

int ove_hal_fb_init(void)
{
	memset(g_fb, 0, sizeof(g_fb));
	g_shm = sh_open("/dev/shm/ove-fb", 7); /* "r+b" */
	return OVE_OK;
}

int ove_hal_fb_get_info(struct ove_fb_info *info)
{
	info->width = FB_W;
	info->height = FB_H;
	info->stride_bytes = FB_W * 2;
	info->fmt = OVE_FB_FMT_RGB565;
	info->smem_len = sizeof(g_fb);
	return OVE_OK;
}

void *ove_hal_fb_buffer(void)
{
	return g_fb;
}

void ove_hal_fb_present(int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	if (g_shm < 0)
		return;
	struct fb_header hdr = {
		.magic = FB_MAGIC,
		.width = FB_W,
		.height = FB_H,
		.format = FB_FORMAT_RGB565,
		.dirty = 1,
	};
	sh_seek(g_shm, 0);
	sh_write(g_shm, &hdr, sizeof(hdr));
	sh_write(g_shm, g_fb, sizeof(g_fb));
}

#endif /* CONFIG_OVE_FB */
