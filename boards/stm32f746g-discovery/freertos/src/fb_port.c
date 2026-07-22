/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746G-Discovery framebuffer backend for the Linux personality (/dev/fb0).
 * The 480x272 RK043FN48H panel is driven by the LTDC scanning a linear RGB565
 * framebuffer out of external SDRAM at LCD_FB_START_ADDRESS (0xC0000000). The
 * portable ove_fb layer + /dev/fb0 class copy guest pixels into that buffer; the
 * LTDC displays them continuously. Mirrors qemu_fb.c on an500, but the "present"
 * is free here (hardware scanout) instead of a shm push.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FB)

#include "ove/hal/hal_fb.h"
#include "ove/types.h"
#include "board_desc.h"

#include "stm32746g_discovery_lcd.h" /* BSP_LCD_*, LCD_FB_START_ADDRESS */
#include "bsp.h"			     /* bsp_sdram_fixup */

#include <string.h>

#define FB_W OVE_DISPLAY_WIDTH
#define FB_H OVE_DISPLAY_HEIGHT

int ove_hal_fb_init(void)
{
	/* BSP_LCD_Init brings up the LTDC + its PLLSAI pixel clock + the panel GPIO
	 * and (re)runs BSP_SDRAM_Init. The framebuffer is LCD_FB_START_ADDRESS, a
	 * region the linker reserves (.lcd_framebuffer) ahead of the personality's
	 * SDRAM pools. This runs at device autoreg, before any program loads, so the
	 * second SDRAM bring-up is harmless (it re-programs the same FMC controller
	 * and keeps refresh alive — no data is live there yet). */
	if (BSP_LCD_Init() != LCD_OK)
		return OVE_ERR_NOT_SUPPORTED;
	/* BSP_LCD_Init re-ran BSP_SDRAM_Init, which reset the FMC SDCR (clearing the
	 * read-pipe delay). Re-apply it now — the LTDC is about to start DMAing the
	 * framebuffer out of this SDRAM, and without the read-pipe margin that
	 * contention corrupts cache-line fills and other accesses to the guest pool. */
	bsp_sdram_fixup();
	BSP_LCD_LayerRgb565Init(0, LCD_FB_START_ADDRESS);
	BSP_LCD_SelectLayer(0);
	BSP_LCD_SetLayerVisible(0, ENABLE);
	BSP_LCD_DisplayOn();
	memset((void *)LCD_FB_START_ADDRESS, 0, (size_t)FB_W * FB_H * 2u);
	return OVE_OK;
}

int ove_hal_fb_get_info(struct ove_fb_info *info)
{
	info->width = FB_W;
	info->height = FB_H;
	info->stride_bytes = FB_W * 2;
	info->fmt = OVE_FB_FMT_RGB565;
	info->smem_len = (uint32_t)FB_W * FB_H * 2u;
	return OVE_OK;
}

void *ove_hal_fb_buffer(void)
{
	return (void *)LCD_FB_START_ADDRESS;
}

void ove_hal_fb_present(void)
{
	/* No-op: the LTDC continuously scans the SDRAM framebuffer, and the
	 * framebuffer @0xC0000000 is non-cacheable (bsp MPU region 0 / the Cortex-M7
	 * background Device map — the guest's cacheable WBWA MPU regions cover only the
	 * program pool, not the fb), so /dev/fb0 writes land straight in SDRAM and
	 * appear on the next LTDC refresh with no cache maintenance — even though the
	 * personality now runs with the D-cache enabled (stm32f7_init.c:72). */
}

#endif /* CONFIG_OVE_FB */
