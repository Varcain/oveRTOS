/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/lvgl_internal.h"
#include "ove/sync.h"
#include "ove_backend_common.h"
#include "lvgl.h"
#include "board_desc.h"

#include <SDL.h>
#include <semaphore.h>

static ove_mutex_t lvgl_mutex;
static volatile int lvgl_ready;
static volatile int sdl_init_pending;
static sem_t sdl_init_sem;

#ifdef CONFIG_OVE_ZERO_HEAP
static ove_mutex_storage_t lvgl_mutex_storage;
#endif

int ove_lvgl_init(void)
{
	int ret;

	if (lvgl_ready) {
		return OVE_OK;
	}

#ifdef CONFIG_OVE_ZERO_HEAP
	ret = ove_mutex_init(&lvgl_mutex, &lvgl_mutex_storage);
#else
	ret = ove_mutex_create(&lvgl_mutex);
#endif
	if (ret != OVE_OK) {
		return ret;
	}

	sem_init(&sdl_init_sem, 0, 0);

	lv_init();

	/* Signal that ove_lvgl_handler() should create the SDL window
	 * on the graphics thread, then wait for it to finish. */
	__sync_synchronize();
	sdl_init_pending = 1;
	lvgl_ready = 1;

	sem_wait(&sdl_init_sem);

	return OVE_OK;
}

void ove_lvgl_lock(void)
{
	if (!lvgl_ready) {
		return;
	}
	ove_mutex_lock(lvgl_mutex, OVE_WAIT_FOREVER);
}

void ove_lvgl_unlock(void)
{
	if (!lvgl_ready) {
		return;
	}
	ove_mutex_unlock(lvgl_mutex);
}

void ove_lvgl_tick(uint32_t ms)
{
	if (!lvgl_ready) {
		return;
	}
	lv_tick_inc(ms);
}

void ove_lvgl_handler(void)
{
	if (!lvgl_ready) {
		return;
	}

	if (sdl_init_pending) {
		/* Create the SDL window on this thread (graphics thread)
		 * so that SDL_PollEvent and rendering happen here too. */
		lv_display_t *disp = lv_sdl_window_create(
			OVE_DISPLAY_WIDTH,
			OVE_DISPLAY_HEIGHT);
		if (disp != NULL) {
			lv_indev_t *mouse = lv_sdl_mouse_create();
			(void)mouse;

			lv_theme_t *th = lv_theme_default_init(
				disp,
				lv_palette_main(LV_PALETTE_BLUE),
				lv_palette_main(LV_PALETTE_RED),
				true, &lv_font_montserrat_32);
			lv_display_set_theme(disp, th);
		}

		__sync_synchronize();
		sdl_init_pending = 0;
		sem_post(&sdl_init_sem);
	}

	lv_timer_handler();
}

