/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C Example Application — zero-heap mode
 *
 * Showcases the zero-heap static-allocation pattern:
 *   - File-scope OVE_QUEUE_DEFINE_STATIC / OVE_MUTEX_DEFINE_STATIC /
 *     OVE_TIMER_DEFINE_STATIC / OVE_THREAD_DEFINE_STATIC declarations.
 *   - Each macro emits a __attribute__((constructor)) that initialises
 *     the handle from caller-owned static storage before ove_main()
 *     runs — no `_create()` symbols are linked in this build, no RTOS
 *     heap involvement at any point.
 *
 * LVGL specifics in zero-heap mode:
 *   - LVGL has no static-allocation API for widgets; lv_*_create()
 *     always allocates the widget struct.  We pin LVGL's TLSF pool to
 *     LV_MEM_SIZE bytes in BSS (CONFIG_LV_MEM_SIZE_KILOBYTES) and
 *     disable expansion (CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=0)
 *     so allocations never touch the system malloc.  This pool lives
 *     in its own BSS region — separate from the RTOS heap that
 *     ove_heap_lock() guards.
 *   - All widget creation happens once in create_ui(), before
 *     ove_run() engages the heap lock.  The UI never grows or shrinks
 *     after that point.
 *   - The dynamic count display uses lv_label_set_text_static() with
 *     a caller-owned buffer — LVGL stores the pointer rather than
 *     duplicating the string, so no realloc on update.
 */

#include "ove/ove.h"
#include "ove/lvgl.h"
#include <stdio.h>

/* --- Shared state --- */

static uint32_t last_value;

static lv_obj_t *title_label;
static lv_obj_t *count_label;
static lv_obj_t *bar;
static char count_buf[32] = "Count: 0"; /* owned by us; pinned via lv_label_set_text_static */
static void ui_timer_cb(ove_timer_t timer, void *user_data);

/* --- Thread entry points (forward declarations) --- */

static void producer_thread(void *arg);
static void consumer_thread(void *arg);
static void graphics_thread(void *arg);

/* --- Statically declared kernel objects ---
 *
 * Each macro expands to:
 *   - static <handle_type> name;
 *   - static ove_*_storage_t _name_storage;
 *   - __attribute__((constructor)) initialiser
 *
 * Initialisation runs before ove_main(); the handles are valid by the
 * time ove_main() begins.  Exactly one instance per declaration — no
 * call-site footgun.
 */
OVE_QUEUE_DEFINE_STATIC(counter_queue, sizeof(uint32_t), 8);
OVE_MUTEX_DEFINE_STATIC(value_mutex);
OVE_TIMER_DEFINE_STATIC(ui_timer, ui_timer_cb, NULL, 200, 0);
OVE_THREAD_DEFINE_STATIC(graphics_thread_handle, 4096, graphics_thread, NULL, OVE_PRIO_HIGH,
			 "graphics");
OVE_THREAD_DEFINE_STATIC(producer_thread_handle, 4096, producer_thread, NULL, OVE_PRIO_NORMAL,
			 "producer");
OVE_THREAD_DEFINE_STATIC(consumer_thread_handle, 4096, consumer_thread, NULL, OVE_PRIO_NORMAL,
			 "consumer");

/* --- Producer thread: generates incrementing counter values --- */

static void producer_thread(void *arg)
{
	(void)arg;
	uint32_t count = 0;

	OVE_LOG_INF("Producer started");

	while (1) {
		++count;
		int ret = ove_queue_send(counter_queue, &count, OVE_MS(1000));
		if (ret != OVE_OK) {
			OVE_LOG_WRN("Producer: queue full, dropped %u", count);
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
		int ret = ove_queue_receive(counter_queue, &val, OVE_WAIT_FOREVER);
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
	uint32_t val;

	ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
	val = last_value;
	ove_mutex_unlock(value_mutex);

	ove_lvgl_lock();
	/* Refresh the static buffer in place.  lv_label_set_text_static()
	 * stored its address once at create_ui() time; calling it again
	 * with the same pointer just triggers a redraw — no allocation. */
	snprintf(count_buf, sizeof(count_buf), "Count: %u", (unsigned int)val);
	lv_label_set_text_static(count_label, count_buf);
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

/* --- create_ui ---
 *
 * Builds the entire UI once at startup.  All lv_*_create() and style
 * calls land in the bounded LV_MEM_SIZE pool.  After this returns no
 * widgets are ever created or destroyed for the lifetime of the app —
 * only existing widgets are updated in place from the timer callback.
 *
 * The two label texts use lv_label_set_text_static() with caller-owned
 * storage:
 *   - APP_TITLE is a string literal (`.rodata`)
 *   - count_buf is a static char array (BSS)
 * Neither path triggers a per-update allocation in lv_label_*.
 */
static void create_ui(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	title_label = lv_label_create(scr);
	lv_label_set_text_static(title_label, APP_TITLE);
	lv_obj_set_style_text_font(title_label, &lv_font_montserrat_32, 0);
	lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 16);

	count_label = lv_label_create(scr);
	lv_label_set_text_static(count_label, count_buf);
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

/* --- App entry point --- */

void ove_main(void)
{
	int ret;

	OVE_LOG_INF("C example (zero-heap mode): init");

	/* Queue, mutex, timer, and threads are statically allocated and
	 * already initialised by OVE_*_DEFINE_STATIC constructors before
	 * ove_main() runs.  No _create() / _destroy() calls in this file —
	 * those symbols don't exist in zero-heap builds. */

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

	OVE_LOG_INF("C example (zero-heap mode): ready");

	ove_run();

	/* Cleanup: only reached if the scheduler returns (POSIX).  The
	 * statically declared handles outlive ove_main(); _deinit() simply
	 * releases the kernel side without freeing the storage. */
	OVE_LOG_INF("C example (zero-heap mode): shutdown");

	ove_timer_stop(ui_timer);
	ove_timer_deinit(ui_timer);
	ove_mutex_deinit(value_mutex);
	ove_queue_deinit(counter_queue);
}
