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
 * by the host viewer (config/scripts/ove-dashboard-bridge.py). Headless runs
 * (no --display) just get a NULL file and present() no-ops.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FB)

#include "ove/hal/hal_fb.h"
#include "ove/types.h"
#include "board_desc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FB_W OVE_DISPLAY_WIDTH
#define FB_H OVE_DISPLAY_HEIGHT
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
static FILE *g_shm;

int ove_hal_fb_init(void)
{
	memset(g_fb, 0, sizeof(g_fb));
	/* Opened via semihosting; NULL when run headless (no --display) — fine. */
	g_shm = fopen("/dev/shm/ove-fb", "r+b");
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

void ove_hal_fb_present(void)
{
	if (!g_shm)
		return;
	struct fb_header hdr = {
		.magic = FB_MAGIC,
		.width = FB_W,
		.height = FB_H,
		.format = FB_FORMAT_RGB565,
		.dirty = 1,
	};
	fseek(g_shm, 0, SEEK_SET);
	fwrite(&hdr, 1, sizeof(hdr), g_shm);
	fwrite(g_fb, 1, sizeof(g_fb), g_shm);
	fflush(g_shm);
}

#endif /* CONFIG_OVE_FB */
