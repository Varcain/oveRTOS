/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "lvgl.h"
#include "stm32746g_discovery_lcd.h"
#include "board_desc.h"
#include <string.h>

#define DISP_HOR_RES OVE_DISPLAY_WIDTH
#define DISP_VER_RES OVE_DISPLAY_HEIGHT
#define BYTES_PER_PIXEL 2

/* Partial draw buffer in internal SRAM (20 lines = 19200 bytes).  Swept on real
 * silicon: 20 lines is the largest that fits SRAM alongside the 96 KB LV_MEM, and
 * beats every SDRAM-placed size (even full-screen), which tops out ~1 FPS lower —
 * the M7's 4 KB D-cache thrashes on a large SDRAM buffer.  10->20 lines: 20->22 FPS. */
#define DRAW_BUF_LINES 20
static uint8_t draw_buf[DISP_HOR_RES * DRAW_BUF_LINES * BYTES_PER_PIXEL]
	__attribute__((aligned(32)));

static void disp_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
	uint32_t w = area->x2 - area->x1 + 1;
	uint32_t h = area->y2 - area->y1 + 1;

	/* Copy each line of the rendered strip into the SDRAM framebuffer.
     * Use lv_memcpy: the toolchain memcpy may emit 4-byte stores which
     * fault on the 2-byte-aligned SDRAM addresses that arise when
     * area->x1 is odd (RGB565 = 2 bytes/pixel). */
	uint8_t *fb = (uint8_t *)LCD_FB_START_ADDRESS;
	for (uint32_t y = 0; y < h; y++) {
		uint32_t fb_offset = ((area->y1 + y) * DISP_HOR_RES + area->x1) * BYTES_PER_PIXEL;
		uint32_t src_offset = y * w * BYTES_PER_PIXEL;
		lv_memcpy(fb + fb_offset, px_map + src_offset, w * BYTES_PER_PIXEL);
	}

	/* Clean D-cache for the written SDRAM region so LTDC sees updated data */
	uint32_t total_bytes = h * DISP_HOR_RES * BYTES_PER_PIXEL;
	SCB_CleanDCache_by_Addr((uint32_t *)(fb + (area->y1 * DISP_HOR_RES * BYTES_PER_PIXEL)),
				total_bytes);

	lv_display_flush_ready(display);
}

void lv_port_disp_hw_init(void)
{
	/* Initialize LCD hardware (SDRAM, LTDC, GPIO, PLLSAI clock) */
	BSP_LCD_Init();
	BSP_LCD_LayerRgb565Init(0, LCD_FB_START_ADDRESS);
	BSP_LCD_SelectLayer(0);
	BSP_LCD_SetLayerVisible(0, ENABLE);
	BSP_LCD_DisplayOn();

	/* Clear framebuffer to black */
	memset((void *)LCD_FB_START_ADDRESS, 0, DISP_HOR_RES * DISP_VER_RES * BYTES_PER_PIXEL);
	SCB_CleanDCache_by_Addr((uint32_t *)LCD_FB_START_ADDRESS,
				DISP_HOR_RES * DISP_VER_RES * BYTES_PER_PIXEL);
}

void lv_port_disp_init(void)
{
	/* Create LVGL display (hardware already initialized by lv_port_disp_hw_init) */
	lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, disp_flush_cb);
}
