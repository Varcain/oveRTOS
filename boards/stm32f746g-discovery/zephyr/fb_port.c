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
 * display_get_framebuffer(); present is a no-op (continuous scanout, D-cache off). The personality's
 * /dev/fb0 copies guest pwrite scanlines into it (privileged 16-bit stores). Mirrors fb_port.c on
 * FreeRTOS / board_init.c on NuttX.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FB)

#include "ove/hal/hal_fb.h"
#include "ove/types.h"
#include "board_desc.h" /* OVE_DISPLAY_WIDTH / OVE_DISPLAY_HEIGHT */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <string.h>
#include <stdint.h>

#define FB_W OVE_DISPLAY_WIDTH
#define FB_H OVE_DISPLAY_HEIGHT

static const struct device *const g_disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

int ove_hal_fb_init(void)
{
	if (!device_is_ready(g_disp))
		return OVE_ERR_NOT_SUPPORTED;
	display_blanking_off(g_disp); /* panel on (disp-on / backlight GPIOs) */
	void *fb = display_get_framebuffer(g_disp);
	if (!fb)
		return OVE_ERR_NOT_SUPPORTED;
	memset(fb, 0, (size_t)FB_W * FB_H * 2u); /* D-cache off → reaches SDRAM */
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
	/* No-op: the LTDC scans the SDRAM framebuffer continuously and the personality keeps the
	 * D-cache off, so /dev/fb0 writes land straight in SDRAM and appear on the next refresh. */
}

#endif /* CONFIG_OVE_FB */
