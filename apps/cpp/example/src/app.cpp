/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Example Application
 *
 * Mirrors apps/c/example structurally; demonstrates the C++ binding's
 * idioms over the same producer/consumer/UI flow:
 *   - file-scope ove::Queue / ove::Mutex / ove::Thread<> / ove::Timer
 *     wrappers (no heap, RAII-managed lifetimes)
 *   - ove::LockGuard for mutex protection
 *   - ove::lvgl::LvglGuard + fluent Label/Bar builders
 *   - OVE_LOG_* macros + OVE_MAIN entry-point macro
 */

#include <ove/ove.hpp>
#include <ove/lvgl.hpp>

namespace lv = ove::lvgl;

/* --- Forward declarations for thread entry points --- */

static void producer_thread(void *arg);
static void consumer_thread(void *arg);
static void graphics_thread(void *arg);
static void ui_timer_cb(ove_timer_t, void *);

/* --- Shared state (file-scope, no heap allocation) --- */

static ove::Queue<uint32_t, 8> counter_queue;
static ove::Mutex value_mutex;
static uint32_t last_value = 0;

static lv::Label count_label{nullptr};
static lv::Bar bar{nullptr};

/* --- Wrappers — constructors call into the kernel at static-init time, */
/*     equivalent to the ove_*_create calls in the C example's ove_main. */

static ove::Timer ui_timer(ui_timer_cb, nullptr, 200);
static ove::Thread<4096> gfx_thread(graphics_thread, nullptr, OVE_PRIO_HIGH, "graphics");
static ove::Thread<4096> prod_thread(producer_thread, nullptr, OVE_PRIO_NORMAL, "producer");
static ove::Thread<4096> cons_thread(consumer_thread, nullptr, OVE_PRIO_NORMAL, "consumer");

/* --- Producer thread: generates incrementing counter values --- */

static void producer_thread(void *arg)
{
	(void)arg;
	uint32_t count = 0;

	OVE_LOG_INF("Producer started");

	while (true) {
		++count;
		int ret = counter_queue.send(count, 1000);
		if (ret != OVE_OK) {
			OVE_LOG_WRN("Producer: queue full, dropped %u", count);
		}
		ove::Thread<>::sleep_ms(500);
	}
}

/* --- Consumer thread: reads values, updates shared state --- */

static void consumer_thread(void *arg)
{
	(void)arg;
	uint32_t val = 0;

	OVE_LOG_INF("Consumer started");

	while (true) {
		int ret = counter_queue.receive(&val, OVE_WAIT_FOREVER);
		if (ret == OVE_OK) {
			{
				ove::LockGuard lock(value_mutex);
				last_value = val;
			}
			if (val % 5 == 0) {
				OVE_LOG_INF("Consumer: count = %u", val);
			}
		}
	}
}

/* --- LVGL UI --- */

static constexpr const char *APP_TITLE =
#if defined(CONFIG_OVE_RTOS_FREERTOS)
	"oveRTOS(FreeRTOS) C++ Demo";
#elif defined(CONFIG_OVE_RTOS_NUTTX)
	"oveRTOS(NuttX) C++ Demo";
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
	"oveRTOS(Zephyr) C++ Demo";
#elif defined(CONFIG_OVE_RTOS_POSIX)
	"oveRTOS(POSIX) C++ Demo";
#else
	"oveRTOS C++ Demo";
#endif

static void create_ui()
{
	auto scr = lv::ObjectView::screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	lv::Label::create(scr)
		.text(APP_TITLE)
		.font(&lv_font_montserrat_32)
		.color(lv_color_white())
		.align(LV_ALIGN_TOP_MID, 0, 16);

	count_label = lv::Label::create(scr)
			      .text("Count: 0")
			      .font(&lv_font_montserrat_14)
			      .color(lv_color_white())
			      .align(LV_ALIGN_TOP_MID, 0, 64);

	bar = lv::Bar::create(scr)
		      .size(200, 16)
		      .range(0, 100)
		      .value(0)
		      .indicator_color(lv_palette_main(LV_PALETTE_BLUE))
		      .radius(8)
		      .align(LV_ALIGN_TOP_MID, 0, 96);
}

static void ui_timer_cb(ove_timer_t, void *)
{
	uint32_t val;
	{
		ove::LockGuard lock(value_mutex);
		val = last_value;
	}

	lv::LvglGuard guard;
	if (count_label)
		count_label.text_fmt("Count: %u", val);
	if (bar)
		bar.value(val % 101, LV_ANIM_ON);
}

static void graphics_thread(void *arg)
{
	(void)arg;

	uint64_t last_us = 0;
	ove_time_get_us(&last_us);

	while (true) {
		uint64_t now_us = 0;
		ove_time_get_us(&now_us);
		uint32_t elapsed_ms = static_cast<uint32_t>((now_us - last_us) / 1000);
		last_us = now_us;

		{
			lv::LvglGuard guard;
			ove_lvgl_tick(elapsed_ms);
			ove_lvgl_handler();
		}
		ove::Thread<>::sleep_ms(33);
	}
}

/* --- App entry point --- */

OVE_MAIN()
{
	OVE_LOG_INF("C++ example: init");

	int ret = ove_lvgl_init();
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to initialize LVGL: %d", ret);
		return;
	}

	{
		lv::LvglGuard guard;
		create_ui();
	}

	ret = ui_timer.start();
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to start UI timer: %d", ret);
		return;
	}

	OVE_LOG_INF("C++ example: ready");

	ove::run();

	OVE_LOG_INF("C++ example: shutdown");
}
