/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <nuttx/config.h>
#include "ove/lvgl_internal.h"
#include "ove_backend_common.h"

#ifdef CONFIG_OVE_LVGL
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <lvgl/lvgl.h>
#include "board_desc.h"

static pthread_mutex_t lvgl_mutex = PTHREAD_MUTEX_INITIALIZER;
static int lvgl_initialized;

/* Board-specific QEMU display port (semihosting framebuffer). */
#ifdef CONFIG_OVE_BOARD_QEMU_MPS2_AN500
extern void lv_port_disp_qemu_init(void);
#endif

/*
 * Partial-buffer display driver for boards with a memory-mapped
 * framebuffer (e.g. STM32F746 LTDC + SDRAM).
 *
 * LVGL's lv_nuttx_init() fbdev path allocates a screen-sized off-screen
 * buffer via malloc() which exceeds internal SRAM on most MCUs.
 * This driver uses a small draw buffer in SRAM and copies rendered
 * strips into the framebuffer, identical to the FreeRTOS approach.
 */
#if defined(OVE_DISPLAY_WIDTH) && defined(OVE_MEMORY_SDRAM_START) && defined(CONFIG_STM32F7_LTDC)

#define FB_HOR_RES OVE_DISPLAY_WIDTH
#define FB_VER_RES OVE_DISPLAY_HEIGHT
#define FB_BPP 2
#define FB_DRAW_LINES 10
#define FB_START_ADDR ((uint8_t *)OVE_MEMORY_SDRAM_START)

static uint8_t nuttx_draw_buf[FB_HOR_RES * FB_DRAW_LINES * FB_BPP] __attribute__((aligned(32)));

static void nuttx_disp_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
	uint32_t w = area->x2 - area->x1 + 1;
	uint32_t h = area->y2 - area->y1 + 1;

	for (uint32_t y = 0; y < h; y++) {
		uint32_t fb_off = ((area->y1 + y) * FB_HOR_RES + area->x1) * FB_BPP;
		uint32_t src_off = y * w * FB_BPP;
		lv_memcpy(FB_START_ADDR + fb_off, px_map + src_off, w * FB_BPP);
	}

	lv_display_flush_ready(display);
}

static lv_display_t *nuttx_fb_display_init(void)
{
	/* Open /dev/fb0 to ensure LTDC hardware is initialized, then close.
	 * The actual rendering goes directly to the SDRAM framebuffer. */
	int fd = open("/dev/fb0", O_RDWR);
	if (fd >= 0) {
		close(fd);
	}

	/* Clear framebuffer */
	memset(FB_START_ADDR, 0, FB_HOR_RES * FB_VER_RES * FB_BPP);

	lv_display_t *disp = lv_display_create(FB_HOR_RES, FB_VER_RES);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, nuttx_draw_buf, NULL, sizeof(nuttx_draw_buf),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, nuttx_disp_flush_cb);
	return disp;
}

#define OVE_NUTTX_HAS_FB_DISPLAY 1
#endif /* STM32 LTDC + SDRAM */

int ove_lvgl_init(void)
{
	lv_display_t *disp = NULL;

	if (__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE)) {
		return OVE_OK;
	}

	lv_init();

#ifdef OVE_NUTTX_HAS_FB_DISPLAY
	/* Direct framebuffer path — avoids lv_nuttx_init() fbdev malloc */
	disp = nuttx_fb_display_init();
#else
	{
		lv_nuttx_dsc_t info;
		lv_nuttx_result_t result;

		lv_nuttx_dsc_init(&info);
		lv_nuttx_init(&info, &result);
		disp = result.disp;
	}

#ifdef CONFIG_OVE_BOARD_QEMU_MPS2_AN500
	if (disp == NULL) {
		lv_port_disp_qemu_init();
		disp = lv_display_get_default();
	}
#endif
#endif /* OVE_NUTTX_HAS_FB_DISPLAY */

	if (disp == NULL) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
					       lv_palette_main(LV_PALETTE_RED), true,
					       &lv_font_montserrat_32);
	lv_display_set_theme(disp, th);

	/*
	 * Publish readiness only after LVGL is fully initialised (release).
	 * The lock/tick/handler entry points no-op until this flag is set, so
	 * an application thread that calls into LVGL before ove_lvgl_init()
	 * completes — e.g. a graphics worker spawned ahead of init — does
	 * nothing instead of driving LVGL before lv_init().
	 */
	__atomic_store_n(&lvgl_initialized, 1, __ATOMIC_RELEASE);

	return OVE_OK;
}

void ove_lvgl_lock(void)
{
	if (!__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE))
		return;
	pthread_mutex_lock(&lvgl_mutex);
}

void ove_lvgl_unlock(void)
{
	if (!__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE))
		return;
	pthread_mutex_unlock(&lvgl_mutex);
}

void ove_lvgl_tick(uint32_t ms)
{
	if (!__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE))
		return;
	lv_tick_inc(ms);
}

void ove_lvgl_handler(void)
{
	if (!__atomic_load_n(&lvgl_initialized, __ATOMIC_ACQUIRE))
		return;
	lv_timer_handler();
}

#else /* !CONFIG_OVE_LVGL */

int ove_lvgl_init(void)
{
	return OVE_OK;
}
void ove_lvgl_lock(void)
{
}
void ove_lvgl_unlock(void)
{
}
void ove_lvgl_tick(uint32_t ms)
{
	(void)ms;
}
void ove_lvgl_handler(void)
{
}

#endif /* CONFIG_OVE_LVGL */
