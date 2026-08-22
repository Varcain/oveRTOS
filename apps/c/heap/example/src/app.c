/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C Example Application — heap mode
 *
 * Showcases the heap-allocating create/destroy API:
 *   - ove_queue_create / ove_queue_destroy
 *   - ove_mutex_create / ove_mutex_destroy
 *   - ove_timer_create / ove_timer_destroy
 *   - ove_thread_create / ove_thread_destroy
 *
 * Handles are allocated from the RTOS heap inside ove_main().  No
 * file-scope storage; producer/consumer/UI counts and queue depth can
 * change at runtime.  Pair with the zeroheap variant (apps/c/zeroheap/
 * example/) which uses OVE_*_DEFINE_STATIC instead.
 *
 * LVGL allocates from its own builtin TLSF pool (see LV_MEM_SIZE).  In
 * heap mode the pool can grow via LV_MEM_POOL_EXPAND_SIZE; widgets and
 * label text buffers may be (re)allocated freely at any time.
 */

#include "ove/ove.h"
#include "ove/lvgl.h"
#include <stdio.h>
#include <stdlib.h>

/* --- Tunables (heap mode lets us pick at runtime) --- */

#define QUEUE_DEPTH 8
#define UI_TIMER_PERIOD_MS 200

/* --- Shared state --- */

static uint32_t last_value;

static lv_obj_t *title_label;
static lv_obj_t *count_label;
static lv_obj_t *bar;

/* --- Primitive handles (assigned by _create() at runtime) --- */

static ove_queue_t counter_queue;
static ove_mutex_t value_mutex;
static ove_timer_t ui_timer;
static ove_thread_t graphics_thread_handle;
static ove_thread_t producer_thread_handle;
static ove_thread_t consumer_thread_handle;

/* --- Producer thread: generates incrementing counter values --- */

static void producer_thread(void *arg)
{
	(void)arg;
	ove_thread_t self = ove_thread_get_self();
	uint32_t count = 0;

	OVE_LOG_INF("Producer started");

	while (!ove_thread_should_stop(self)) {
		++count;
		int ret = ove_queue_send(counter_queue, &count, OVE_MS(1000));
		if (ret != OVE_OK) {
			OVE_LOG_WRN("Producer: queue full, dropped %u", (unsigned int)count);
		}
		ove_thread_sleep_ms(500);
	}
}

/* --- Consumer thread: reads values, updates shared state --- */

static void consumer_thread(void *arg)
{
	(void)arg;
	ove_thread_t self = ove_thread_get_self();
	uint32_t val = 0;

	OVE_LOG_INF("Consumer started");

	while (!ove_thread_should_stop(self)) {
		int ret = ove_queue_receive(counter_queue, &val, OVE_WAIT_FOREVER);
		if (ret == OVE_OK) {
			ret = ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
			if (ret != OVE_OK) {
				OVE_LOG_ERR("Consumer: mutex lock failed: %d", ret);
				continue;
			}
			last_value = val;
			ove_mutex_unlock(value_mutex);

			if (val % 5 == 0) {
				OVE_LOG_INF("Consumer: count = %u", (unsigned int)val);
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

	if (ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER) != OVE_OK)
		return;
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
	ove_thread_t self = ove_thread_get_self();
	uint64_t last_us = 0;

	ove_time_get_us(&last_us);

	while (!ove_thread_should_stop(self)) {
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
	lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
	lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 96);
}

static void stop_thread(ove_thread_t thread)
{
	ove_thread_request_stop(thread);
	ove_thread_destroy(thread);
}

static void stop_consumer(void)
{
	uint32_t wake = 0;
	ove_thread_request_stop(consumer_thread_handle);
	int wake_rc = ove_queue_send(counter_queue, &wake, 0);
	(void)wake_rc; /* A full queue already makes the consumer runnable. */
	ove_thread_destroy(consumer_thread_handle);
}

/* --- App entry point --- */

void ove_main(void)
{
	int ret;

	OVE_LOG_INF("C example (heap mode): init");

	/* Heap-allocate kernel objects via the _create() API. */
	ret = ove_queue_create(&counter_queue, sizeof(uint32_t), QUEUE_DEPTH);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to create queue: %d", ret);
		return;
	}

	ret = ove_mutex_create(&value_mutex);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to create mutex: %d", ret);
		goto cleanup_queue;
	}

	ret = ove_timer_create(&ui_timer, ui_timer_cb, NULL, UI_TIMER_PERIOD_MS, 0);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to create UI timer: %d", ret);
		goto cleanup_mutex;
	}

	/* No worker may enter LVGL before the library and UI are ready. */
	ret = ove_lvgl_init();
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to initialize LVGL: %d", ret);
		goto cleanup_timer;
	}
	ove_lvgl_lock();
	create_ui();
	ove_lvgl_unlock();

	ret = ove_thread_create(&graphics_thread_handle, "graphics", graphics_thread, NULL,
				OVE_PRIO_HIGH, 4096);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to spawn graphics: %d", ret);
		goto cleanup_timer;
	}
	ret = ove_thread_create(&producer_thread_handle, "producer", producer_thread, NULL,
				OVE_PRIO_NORMAL, 4096);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to spawn producer: %d", ret);
		goto cleanup_graphics;
	}
	ret = ove_thread_create(&consumer_thread_handle, "consumer", consumer_thread, NULL,
				OVE_PRIO_NORMAL, 4096);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to spawn consumer: %d", ret);
		goto cleanup_producer;
	}

	ret = ove_timer_start(ui_timer);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to start UI timer: %d", ret);
		goto cleanup_consumer;
	}

	OVE_LOG_INF("C example (heap mode): ready");

	ove_run();

	/* Cleanup path — only reached when the scheduler returns (POSIX). */
	OVE_LOG_INF("C example (heap mode): shutdown");

	ove_timer_stop(ui_timer);

cleanup_consumer:
	stop_consumer();
cleanup_producer:
	stop_thread(producer_thread_handle);
cleanup_graphics:
	stop_thread(graphics_thread_handle);
cleanup_timer:
	ove_timer_destroy(ui_timer);
cleanup_mutex:
	ove_mutex_destroy(value_mutex);
cleanup_queue:
	ove_queue_destroy(counter_queue);
}
