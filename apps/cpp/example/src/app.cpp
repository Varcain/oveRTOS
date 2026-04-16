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
 * Demonstrates the ove::lvgl C++20 wrapper:
 *   - LvglGuard RAII locking
 *   - Fluent widget builders (Label, Bar, Box)
 *   - Reactive State<T> with observer bindings
 *   - Component<T> CRTP composition
 *   - Layout helpers (vbox, hbox)
 */

#include <ove/ove.hpp>
#include <ove/lvgl.hpp>
#include "generated_images/lvgl_images.h"

namespace lv = ove::lvgl;

/* --- Forward declarations for thread entry points --- */

static void producer_thread(void *arg);
static void consumer_thread(void *arg);

/* --- Shared state (file-scope, no heap allocation) --- */

static ove::Queue<uint32_t, 8> counter_queue;
static ove::Mutex value_mutex;
static uint32_t last_value = 0;

static void ui_timer_cb(ove_timer_t, void *);
static void graphics_thread(void *arg);

static ove::Timer ui_timer(ui_timer_cb, nullptr, 200);
static ove::Thread<4096> gfx_thread(graphics_thread, nullptr,
					 OVE_PRIO_HIGH, "graphics");

static ove::Thread<4096> prod_thread(producer_thread, nullptr,
					  OVE_PRIO_NORMAL, "producer");
static ove::Thread<4096> cons_thread(consumer_thread, nullptr,
					  OVE_PRIO_NORMAL, "consumer");

/* ================================================================== */
/*  CounterComponent — demonstrates Component<T>, State, bind_text    */
/* ================================================================== */

class CounterComponent : public lv::Component<CounterComponent> {
public:
#if LV_USE_OBSERVER
	lv::State<int> m_count{0};
#else
	lv::Label m_count_label{nullptr};
#endif
	lv::Bar m_bar{nullptr};

#if defined(CONFIG_OVE_RTOS_FREERTOS)
#define APP_TITLE "oveRTOS(FreeRTOS) C++ Demo"
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#define APP_TITLE "oveRTOS(NuttX) C++ Demo"
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#define APP_TITLE "oveRTOS(Zephyr) C++ Demo"
#elif defined(CONFIG_OVE_RTOS_POSIX)
#define APP_TITLE "oveRTOS(POSIX) C++ Demo"
#else
#define APP_TITLE "oveRTOS C++ Demo"
#endif

	lv::ObjectView build(lv::ObjectView parent) {
		auto root = lv::Box::create(parent)
			.size(LV_PCT(100), LV_PCT(100))
			.bg_color(lv_color_black())
			.bg_opa(LV_OPA_COVER)
			.border_width(0)
			.pad_all(0);

		/* Title */
		lv::Label::create(root)
			.text(APP_TITLE)
			.font(&lv_font_montserrat_32)
			.color(lv_color_white())
			.align(LV_ALIGN_TOP_MID, 0, 16);

		/* Counter label — reactive binding or manual update */
#if LV_USE_OBSERVER
		lv::Label::create(root)
			.bind_text(m_count, "Count: %d")
			.font(&lv_font_montserrat_14)
			.color(lv_color_white())
			.align(LV_ALIGN_TOP_MID, 0, 64);
#else
		m_count_label = lv::Label::create(root)
			.text("Count: 0")
			.font(&lv_font_montserrat_14)
			.color(lv_color_white())
			.align(LV_ALIGN_TOP_MID, 0, 64);
#endif

		/* Progress bar */
		m_bar = lv::Bar::create(root)
			.size(200, 16)
			.range(0, 100)
			.value(0)
			.indicator_color(lv_palette_main(LV_PALETTE_BLUE))
			.radius(8)
			.align(LV_ALIGN_TOP_MID, 0, 96);

		/* Tier S widget smoke test — Slider, Button, Switch, Arc */
		lv::Slider::create(root)
			.size(200, 12)
			.range(0, 100)
			.value(50)
			.indicator_color(lv_palette_main(LV_PALETTE_GREEN))
			.align(LV_ALIGN_TOP_MID, 0, 128);

		auto btn = lv::Button::create(root)
			.size(96, 32)
			.align(LV_ALIGN_TOP_LEFT, 16, 156);
		lv::Label::create(btn)
			.text("Button")
			.color(lv_color_white())
			.center();

		lv::Switch::create(root)
			.align(LV_ALIGN_TOP_RIGHT, -16, 156);

		lv::Arc::create(root)
			.size(72, 72)
			.range(0, 100)
			.value(75)
			.indicator_color(lv_palette_main(LV_PALETTE_ORANGE))
			.align(LV_ALIGN_TOP_MID, 0, 196);

		/* Logo image from the build-time PNG → C array pipeline */
		lv::Image::create(root)
			.src(&logo)
			.align(LV_ALIGN_BOTTOM_RIGHT, -8, -8);

		return root;
	}

	void update(uint32_t val) {
#if LV_USE_OBSERVER
		m_count.set(static_cast<int32_t>(val));
#else
		if (m_count_label)
			m_count_label.text_fmt("Count: %u", val);
#endif
		if (m_bar)
			m_bar.value(val % 101, LV_ANIM_ON);
	}
};

static CounterComponent counter_component;

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

static void ui_timer_cb(ove_timer_t, void *)
{
    uint32_t val;
    {
        ove::LockGuard lock(value_mutex);
        val = last_value;
    }

    lv::LvglGuard guard;
    counter_component.update(val);
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

        ove_lvgl_lock();
        ove_lvgl_tick(elapsed_ms);
        ove_lvgl_handler();
        ove_lvgl_unlock();

        ove::Thread<>::sleep_ms(33);
    }
}

/* --- App entry point --- */

OVE_MAIN()
{
    OVE_LOG_INF("C++ example: init");

    /* Initialize LVGL and create UI */
    int ret = ove_lvgl_init();
    if (ret != OVE_OK) {
        OVE_LOG_ERR("Failed to initialize LVGL: %d", ret);
        return;
    }

    {
        lv::LvglGuard guard;
        counter_component.mount(lv::ObjectView::screen_active());
    }

    ret = ui_timer.start();
    if (ret != OVE_OK) {
        OVE_LOG_ERR("Failed to start UI timer: %d", ret);
        return;
    }

    OVE_LOG_INF("C++ example: ready");

    ove::run();

    /* Cleanup (only reached if scheduler returns, e.g. POSIX) */
    OVE_LOG_INF("C++ example: shutdown");
    {
        lv::LvglGuard guard;
        counter_component.unmount();
    }
}
