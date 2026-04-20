/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C Example Application
 *
 * Demonstrates the ove C API directly:
 *   - ove_queue_* for inter-thread communication
 *   - ove_mutex_* for shared state protection
 *   - ove_timer_* for periodic callbacks
 *   - ove_lvgl_* for LVGL display (when enabled)
 *   - OVE_LOG_* macros for logging
 *
 * Uses the unified C API which works transparently in both heap
 * and zero-heap builds.
 */

#include "ove/ove.h"
#include "ove/lvgl.h"
#include <stdio.h>

/* --- Shared state --- */

static uint32_t last_value;

static lv_obj_t *title_label;
static lv_obj_t *count_label;
static lv_obj_t *bar;
static void ui_timer_cb(ove_timer_t timer, void *user_data);

/* --- Thread entry points (forward declarations) --- */

static void producer_thread(void *arg);
static void consumer_thread(void *arg);
static void graphics_thread(void *arg);

/* --- Primitive handles --- */

static ove_queue_t counter_queue;
static ove_mutex_t value_mutex;
static ove_timer_t ui_timer;

/* --- Producer thread: generates incrementing counter values --- */

static void producer_thread(void *arg)
{
	(void)arg;
	uint32_t count = 0;

	OVE_LOG_INF("Producer started");

	while (1) {
		++count;
		int ret = ove_queue_send(counter_queue, &count, 1000);
		if (ret != OVE_OK) {
			OVE_LOG_WRN("Producer: queue full, dropped %u",
					count);
		}
		ove_thread_sleep_ms(500);
	}
}

/* --- Consumer thread: reads values, updates shared state --- */

static void consumer_thread(void *arg)
{
	(void)arg;
	uint32_t val = 0;

	OVE_LOG_INF("Consumer started");

	while (1) {
		int ret = ove_queue_receive(counter_queue, &val,
						OVE_WAIT_FOREVER);
		if (ret == OVE_OK) {
			ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
			last_value = val;
			ove_mutex_unlock(value_mutex);

			if (val % 5 == 0) {
				OVE_LOG_INF("Consumer: count = %u", val);
			}
		}
	}
}

/* --- LVGL UI --- */

static void ui_timer_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
	char buf[32];
	uint32_t val;

	ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
	val = last_value;
	ove_mutex_unlock(value_mutex);

	ove_lvgl_lock();
	snprintf(buf, sizeof(buf), "Count: %u", (unsigned int)val);
	lv_label_set_text(count_label, buf);
	lv_bar_set_value(bar, (int32_t)(val % 101), LV_ANIM_ON);
	ove_lvgl_unlock();
}

static void graphics_thread(void *arg)
{
	(void)arg;
	uint64_t last_us = 0;

	ove_time_get_us(&last_us);

	while (1) {
		uint64_t now_us = 0;
		uint32_t elapsed_ms;

		ove_time_get_us(&now_us);
		elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
		last_us = now_us;

		ove_lvgl_lock();
		ove_lvgl_tick(elapsed_ms);
		ove_lvgl_handler();
		ove_lvgl_unlock();

		ove_thread_sleep_ms(33);
	}
}

#if defined(CONFIG_OVE_RTOS_FREERTOS)
#define APP_TITLE "oveRTOS(FreeRTOS) C Demo"
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#define APP_TITLE "oveRTOS(NuttX) C Demo"
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#define APP_TITLE "oveRTOS(Zephyr) C Demo"
#elif defined(CONFIG_OVE_RTOS_POSIX)
#define APP_TITLE "oveRTOS(POSIX) C Demo"
#else
#define APP_TITLE "oveRTOS C Demo"
#endif

static void create_ui(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	title_label = lv_label_create(scr);
	lv_label_set_text(title_label, APP_TITLE);
	lv_obj_set_style_text_font(title_label, &lv_font_montserrat_32, 0);
	lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 16);

	count_label = lv_label_create(scr);
	lv_label_set_text(count_label, "Count: 0");
	lv_obj_set_style_text_font(count_label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(count_label, lv_color_white(), 0);
	lv_obj_align(count_label, LV_ALIGN_TOP_MID, 0, 64);

	bar = lv_bar_create(scr);
	lv_obj_set_size(bar, 200, 16);
	lv_bar_set_range(bar, 0, 100);
	lv_bar_set_value(bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(bar, 8, 0);
	lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_BLUE),
				  LV_PART_INDICATOR);
	lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 96);
}

/* --- App entry point --- */

void ove_main(void)
{
	int ret;
	ove_thread_t thread_handle;

	(void)ret;

	OVE_LOG_INF("C example: init");

	/* Create RTOS primitives */
	ret = ove_queue_create(&counter_queue, sizeof(uint32_t), 8);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to create queue: %d", ret);
		return;
	}

	ret = ove_mutex_create(&value_mutex);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to create mutex: %d", ret);
		return;
	}

	ret = ove_timer_create(&ui_timer, ui_timer_cb, NULL, 200, 0);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to create UI timer: %d", ret);
		return;
	}

	/* Create threads */
	{
		struct ove_thread_desc desc = {
			.name = "graphics",
			.entry = graphics_thread,
			.arg = NULL,
			.priority = OVE_PRIO_HIGH,
		};
		ret = ove_thread_create(&thread_handle, 4096, &desc);
		if (ret != OVE_OK) {
			OVE_LOG_ERR("Failed to create thread '%s': %d",
					desc.name, ret);
			return;
		}
	}

	{
		struct ove_thread_desc desc = {
			.name = "producer",
			.entry = producer_thread,
			.arg = NULL,
			.priority = OVE_PRIO_NORMAL,
		};
		ret = ove_thread_create(&thread_handle, 4096, &desc);
		if (ret != OVE_OK) {
			OVE_LOG_ERR("Failed to create thread '%s': %d",
					desc.name, ret);
			return;
		}
	}

	{
		struct ove_thread_desc desc = {
			.name = "consumer",
			.entry = consumer_thread,
			.arg = NULL,
			.priority = OVE_PRIO_NORMAL,
		};
		ret = ove_thread_create(&thread_handle, 4096, &desc);
		if (ret != OVE_OK) {
			OVE_LOG_ERR("Failed to create thread '%s': %d",
					desc.name, ret);
			return;
		}
	}

	/* Initialize LVGL and create UI */
	ret = ove_lvgl_init();
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to initialize LVGL: %d", ret);
		return;
	}

	ove_lvgl_lock();
	create_ui();
	ove_lvgl_unlock();

	ret = ove_timer_start(ui_timer);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to start UI timer: %d", ret);
		return;
	}

	OVE_LOG_INF("C example: ready");

	ove_run();

	/* Cleanup (only reached if scheduler returns, e.g. POSIX) */
	OVE_LOG_INF("C example: shutdown");

	ove_timer_stop(ui_timer);
	ove_timer_destroy(ui_timer);

	ove_mutex_destroy(value_mutex);
	ove_queue_destroy(counter_queue);
}
