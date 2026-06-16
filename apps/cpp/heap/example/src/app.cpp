/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Example Application — heap mode
 *
 * Showcases idiomatic heap-mode C++ over the same producer/consumer/UI
 * flow as apps/c/heap/example:
 *   - std::unique_ptr<ove::*> for RAII-managed heap-owned wrappers
 *   - std::make_unique to allocate kernel handles inside ove_main()
 *   - All ove::Queue / ove::Mutex / ove::Timer / ove::Thread instances
 *     live on the heap; the wrapper struct is allocated by std::new and
 *     the underlying kernel object is allocated by ove_*_create.
 *   - Pair with the zeroheap variant (apps/cpp/zeroheap/example/) which
 *     uses file-scope static wrappers with embedded storage.
 *
 * LVGL operates from its own builtin TLSF pool (LV_MEM_SIZE).  In heap
 * mode the pool can grow via LV_MEM_POOL_EXPAND_SIZE; widgets and label
 * text buffers may be (re)allocated freely at any time.
 */

#include <ove/ove.hpp>
#include <ove/lvgl.hpp>
#include <memory>

namespace lv = ove::lvgl;

/* --- Forward declarations for thread entry points --- */

static void producer_thread(ove::stop_token st);
static void consumer_thread(ove::stop_token st);
static void graphics_thread(ove::stop_token st);
static void ui_timer_cb(ove_timer_t, void *);

/* --- Heap-owned shared state ---
 *
 * The pointers themselves are file-scope (so the C-callback thread
 * entry points can reach them), but the underlying ove::* objects are
 * heap-allocated inside ove_main() via std::make_unique.  The unique_ptr
 * destructors at program shutdown invoke ove_*_destroy on the kernel
 * handle and free the wrapper.
 */
static std::unique_ptr<ove::Queue<uint32_t, 8>> counter_queue;
static std::unique_ptr<ove::Mutex> value_mutex;
static std::unique_ptr<ove::Timer> ui_timer;
static std::unique_ptr<ove::Thread<4096>> gfx_thread;
static std::unique_ptr<ove::Thread<4096>> prod_thread;
static std::unique_ptr<ove::Thread<4096>> cons_thread;

static uint32_t last_value = 0;

static lv::Label count_label{nullptr};
static lv::Bar bar{nullptr};

/* --- Producer thread: generates incrementing counter values --- */

static void producer_thread(ove::stop_token st)
{
	uint32_t count = 0;

	OVE_LOG_INF("Producer started");

	while (!st.stop_requested()) {
		++count;
		/* 1 s timeout per send; switch on the Result to keep the error
		 * specificity the binding provides. */
		if (auto r = counter_queue->try_send_for(count, std::chrono::milliseconds{1000});
		    !r) {
			if (r.error() == ove::Error::QueueFull) {
				OVE_LOG_WRN("Producer: queue full, dropped %u", count);
			} else {
				OVE_LOG_WRN("Producer: send failed (%d), dropped %u",
					    static_cast<int>(r.error()), count);
			}
		}
		ove::this_thread::sleep_ms(500);
	}
}

/* --- Consumer thread: reads values, updates shared state --- */

static void consumer_thread(ove::stop_token st)
{
	uint32_t val = 0;

	OVE_LOG_INF("Consumer started");

	while (!st.stop_requested()) {
		/* Bounded receive so the loop re-checks the stop token instead
		 * of blocking forever (clean cooperative shutdown). */
		if (!counter_queue->try_receive_for(val, std::chrono::milliseconds{500})) {
			continue;
		}
		{
			ove::LockGuard lock(*value_mutex);
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
		ove::LockGuard lock(*value_mutex);
		val = last_value;
	}

	lv::LvglGuard guard;
	if (count_label)
		count_label.text_fmt("Count: %u", val);
	if (bar)
		bar.value(val % 101, LV_ANIM_ON);
}

static void graphics_thread(ove::stop_token st)
{
	uint64_t last_us = ove::time::get_us().value_or(0);

	while (!st.stop_requested()) {
		uint64_t now_us = ove::time::get_us().value_or(last_us);
		uint32_t elapsed_ms = static_cast<uint32_t>((now_us - last_us) / 1000);
		last_us = now_us;

		{
			lv::LvglGuard guard;
			lv::tick(elapsed_ms);
			lv::handler();
		}
		ove::this_thread::sleep_ms(33);
	}
}

/* --- App entry point --- */

OVE_MAIN()
{
	OVE_LOG_INF("C++ example (heap mode): init");

	/* Heap-allocate the kernel-object wrappers via std::make_unique.
	 * Each `make_unique` call performs two heap allocations:
	 *   1. operator new for the C++ wrapper (~ pointer-sized)
	 *   2. ove_*_create internally for the kernel object
	 * unique_ptr destruction at program shutdown reverses both. */
	counter_queue = std::make_unique<ove::Queue<uint32_t, 8>>();
	value_mutex = std::make_unique<ove::Mutex>();
	ui_timer = std::make_unique<ove::Timer>(ui_timer_cb, nullptr, 200);

	gfx_thread =
		std::make_unique<ove::Thread<4096>>(graphics_thread, OVE_PRIO_HIGH, "graphics");
	prod_thread =
		std::make_unique<ove::Thread<4096>>(producer_thread, OVE_PRIO_NORMAL, "producer");
	cons_thread =
		std::make_unique<ove::Thread<4096>>(consumer_thread, OVE_PRIO_NORMAL, "consumer");

	if (int rc = lv::init(); rc != OVE_OK) {
		OVE_LOG_ERR("Failed to initialize LVGL: %d", rc);
		return;
	}

	{
		lv::LvglGuard guard;
		create_ui();
	}

	if (auto r = ui_timer->start(); !r) {
		OVE_LOG_ERR("Failed to start UI timer: %d", static_cast<int>(r.error()));
		return;
	}

	OVE_LOG_INF("C++ example (heap mode): ready");

	ove::run();

	/* unique_ptrs go out of scope here on platforms where ove::run()
	 * returns (POSIX); on bare-metal targets it never returns. */
	OVE_LOG_INF("C++ example (heap mode): shutdown");
}
