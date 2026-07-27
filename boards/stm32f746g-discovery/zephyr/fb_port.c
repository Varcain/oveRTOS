/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746G-Discovery Zephyr framebuffer backend for the Linux personality (/dev/fb0).
 * Zephyr's own stm32_ltdc display driver frames a linear 480x272 RGB565 buffer in the SDRAM1
 * region and scans it out continuously, so this backend hands the personality that buffer via
 * display_get_framebuffer(). The personality's /dev/fb0 copies guest pwrite scanlines into the
 * cacheable CPU view; present explicitly flushes those writes for LTDC scanout. Mirrors fb_port.c
 * on FreeRTOS / board_init.c on NuttX.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FB)

#include "ove/hal/hal_fb.h"
#include "ove/types.h"
#include "board_desc.h" /* OVE_DISPLAY_WIDTH / OVE_DISPLAY_HEIGHT */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/cache.h>
#include <string.h>
#include <stdint.h>

#define FB_W OVE_DISPLAY_WIDTH
#define FB_H OVE_DISPLAY_HEIGHT

static const struct device *const g_disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

int ove_hal_fb_init(void)
{
	if (!device_is_ready(g_disp))
		return OVE_ERR_NOT_SUPPORTED;
	void *fb = display_get_framebuffer(g_disp);
	if (!fb)
		return OVE_ERR_NOT_SUPPORTED;
	memset(fb, 0, (size_t)FB_W * FB_H * 2u);
	sys_cache_data_flush_range(fb, (size_t)FB_W * FB_H * 2u);
	display_blanking_off(g_disp); /* panel on after the cleared buffer is visible to LTDC */
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
	return display_get_framebuffer(g_disp);
}

void ove_hal_fb_present(void)
{
	/* Clean (flush) the framebuffer's dirty D-cache lines to SDRAM. The privileged /dev/fb0
	 * writer and the guest both see SDRAM through Normal WBWA mappings, so pixels can remain in
	 * cache while the LTDC bus master reads physical SDRAM. Called at ~30 Hz from the run-loop
	 * tick, which coalesces scanline writes into one push. The CPU-only guest program pool needs
	 * no such maintenance because coordinator and guest share one coherent cacheable view. */
	void *fb = display_get_framebuffer(g_disp);
	if (fb)
		sys_cache_data_flush_range(fb, (size_t)FB_W * FB_H * 2u);
}

#endif /* CONFIG_OVE_FB */
