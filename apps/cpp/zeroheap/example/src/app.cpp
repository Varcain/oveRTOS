/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Example Application — zero-heap mode
 *
 * Showcases the zero-heap C++ pattern over the same producer/consumer/UI
 * flow as apps/c/zeroheap/example:
 *   - File-scope `ove::Queue<T,N>`, `ove::Mutex`, `ove::Timer`,
 *     `ove::Thread<StackSize>` instances.  In zero-heap mode the wrapper
 *     classes carry the kernel-object storage (and thread stack) inline
 *     as struct members; their constructors call `ove_*_init` with
 *     pointers into those members — no heap, no operator new.
 *   - Move/copy operations are deleted on every wrapper in zero-heap
 *     mode (the kernel holds pointers into &storage_), so each instance
 *     is structurally pinned to its file-scope address.
 *   - The constructors run during static initialisation, before
 *     OVE_MAIN() is entered.
 *
 * LVGL specifics (same as the C zero-heap example):
 *   - LVGL has no static-allocation API for widgets.  We pin LVGL's
 *     TLSF pool to LV_MEM_SIZE bytes in BSS and disable expansion
 *     (LV_MEM_POOL_EXPAND_SIZE = 0) so allocations never touch the
 *     system malloc.  This pool is separate from the RTOS heap that
 *     ove_heap_lock() guards.
 *   - All widget creation happens once in create_ui() before ove::run()
 *     engages the heap lock.  Label text uses lv::Label::text_static
 *     with caller-owned buffers — no per-update realloc.
 */

#include <ove/ove.hpp>
#include <ove/lvgl.hpp>
#include <cstdio>

namespace lv = ove::lvgl;

/* --- Forward declarations for thread entry points --- */

static void producer_thread(void *arg);
static void consumer_thread(void *arg);
static void graphics_thread(void *arg);
static void ui_timer_cb(ove_timer_t, void *);

/* --- File-scope statically-allocated kernel objects ---
 *
 * Each wrapper's constructor (called during static init) invokes the
 * underlying `ove_*_init` with caller-owned storage that lives inside
 * the wrapper itself.  Order of construction follows declaration order.
 */
static ove::Queue<uint32_t, 8> counter_queue;
static ove::Mutex value_mutex;
static uint32_t last_value = 0;

static lv::Label count_label{nullptr};
static lv::Bar bar{nullptr};
static char count_buf[32] = "Count: 0"; /* pinned via lv::Label::text_static */

/* Timer + threads — wrappers embed storage in zero-heap mode. */
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
		if (!counter_queue.try_send_for(count, std::chrono::milliseconds{1000})) {
			OVE_LOG_WRN("Producer: queue full, dropped %u", count);
		}
		ove::this_thread::sleep_ms(500);
	}
}

/* --- Consumer thread: reads values, updates shared state --- */

static void consumer_thread(void *arg)
{
	(void)arg;
	uint32_t val = 0;

	OVE_LOG_INF("Consumer started");

	while (true) {
		counter_queue.receive(val);
		{
			ove::LockGuard lock(value_mutex);
			last_value = val;
		}
		if (val % 5 == 0) {
			OVE_LOG_INF("Consumer: count = %u", val);
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

/* Builds the entire UI once at startup.  After this returns, no
 * widgets are created or destroyed for the lifetime of the app — only
 * the count label's static buffer is updated in place.  All label text
 * uses text_static with caller-owned storage so no per-update realloc. */
static void create_ui()
{
	auto scr = lv::ObjectView::screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	lv::Label::create(scr)
		.text_static(APP_TITLE)
		.font(&lv_font_montserrat_32)
		.color(lv_color_white())
		.align(LV_ALIGN_TOP_MID, 0, 16);

	count_label = lv::Label::create(scr)
			      .text_static(count_buf)
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
	/* Refresh the static buffer in place; LVGL stored its address
	 * once at create_ui() time and just redraws on the next call. */
	std::snprintf(count_buf, sizeof(count_buf), "Count: %u", val);
	if (count_label)
		count_label.text_static(count_buf);
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
		ove::this_thread::sleep_ms(33);
	}
}

/* --- App entry point --- */

OVE_MAIN()
{
	OVE_LOG_INF("C++ example (zero-heap mode): init");

	/* All ove::* wrappers are already constructed by static init.  No
	 * heap allocations from this point on. */

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

	OVE_LOG_INF("C++ example (zero-heap mode): ready");

	ove::run();

	OVE_LOG_INF("C++ example (zero-heap mode): shutdown");
}
