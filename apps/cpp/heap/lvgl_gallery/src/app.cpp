/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS LVGL Gallery (C++) — one widget per page.
 *
 * Navigation: top bar with  < title (N/22) >  arrows.
 * Each page builds exactly one widget (or a closely-related pair like
 * Textarea + Keyboard) centered in the content area.
 *
 * Timer-driven widgets (Bar, Arc, Led, Chart) are stored in statics
 * that the tick callback checks; they're set when their page is built
 * and nulled when the user navigates away.
 */

#include <ove/ove.hpp>
#include <ove/lvgl.hpp>
#include "generated_images/lvgl_images.h"
#include <cstdio>
#include <memory>
#include <etl/string.h>
#include <etl/string_stream.h>

namespace lv = ove::lvgl;

/* ── Forward declarations ─────────────────────────────────────────── */

static void graphics_thread(void *arg);
static void create_ui();
static void tick_cb(lv_timer_t *);
static void rebuild_page();

/* Heap-mode graphics thread is allocated inside OVE_MAIN(). */

/* ── Timer-driven widget slots (set by page builder, null otherwise) */

static lv::Bar g_bar{nullptr};
static lv::Arc g_arc{nullptr};
static lv::Led g_led{nullptr};
static lv::Chart g_chart{nullptr};
static lv::Chart::Series g_series{};

#if LV_USE_OBSERVER
static lv::State<int> g_counter{0};
#endif

/* Canvas buffer (persists across page switches). */
static uint8_t g_canvas_buf[64 * 64 * 4];

/* ── Navigation state ─────────────────────────────────────────────── */

static lv::Box g_content{nullptr};
static lv::Label g_title_label{nullptr};
static int g_page = 0;

/* ── Page builders (one per widget) ───────────────────────────────── */

static void page_label(lv::ObjectView p)
{
	auto lbl = lv::Label::create(p)
			   .text("Hello, oveRTOS!")
			   .font(&lv_font_montserrat_32)
			   .color(lv_color_white());
	lbl.center();
#if LV_USE_OBSERVER
	lv::Label::create(p)
		.bind_text(g_counter, "Tick: %d")
		.font(&lv_font_montserrat_14)
		.color(lv_color_hex(0x888888))
		.align(LV_ALIGN_BOTTOM_MID, 0, -16);
#endif
}

static void page_button(lv::ObjectView p)
{
	auto btn = lv::Button::create(p).size(160, 48).toggle_mode(true);
	lv::Label::create(btn).text("Toggle me").center();
	btn.center();
}

static void page_switch(lv::ObjectView p)
{
	lv::Switch::create(p).checked(true).center();
}

static void page_checkbox(lv::ObjectView p)
{
	lv::Checkbox::create(p)
		.text("Enable option")
		.checked(true)
		.text_color(lv_color_white())
		.center();
}

static void page_bar(lv::ObjectView p)
{
	g_bar = lv::Bar::create(p)
			.size(300, 20)
			.range(0, 100)
			.indicator_color(lv_palette_main(LV_PALETTE_BLUE))
			.radius(10)
			.center();
}

static void page_slider(lv::ObjectView p)
{
	lv::Slider::create(p)
		.size(300, 20)
		.range(0, 100)
		.value(50)
		.indicator_color(lv_palette_main(LV_PALETTE_GREEN))
		.center();
}

static void page_arc(lv::ObjectView p)
{
	g_arc = lv::Arc::create(p)
			.size(120, 120)
			.range(0, 100)
			.value(40)
			.indicator_color(lv_palette_main(LV_PALETTE_ORANGE))
			.center();
}

static void page_spinner(lv::ObjectView p)
{
	lv::Spinner::create(p).size(80, 80).anim_params(1000, 60).center();
}

static void page_led(lv::ObjectView p)
{
	g_led = lv::Led::create(p).size(60, 60);
	g_led.color(lv_palette_main(LV_PALETTE_RED));
	g_led.center();
}

static void page_dropdown(lv::ObjectView p)
{
	lv::Dropdown::create(p)
		.options_static("Red\nGreen\nBlue\nYellow")
		.selected(2)
		.width(200)
		.center();
}

static void page_roller(lv::ObjectView p)
{
	lv::Roller::create(p)
		.options("Mon\nTue\nWed\nThu\nFri\nSat\nSun", LV_ROLLER_MODE_NORMAL)
		.visible_row_count(4)
		.width(140)
		.center();
}

static void page_spinbox(lv::ObjectView p)
{
	auto sb = lv::Spinbox::create(p).width(200);
	sb.digit_format(4, 2).range(-9999, 9999).step(1).value(42);
	sb.center();
}

static void page_textarea(lv::ObjectView p)
{
	lv_obj_set_flex_flow(p.get(), LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_gap(p.get(), 8, LV_PART_MAIN);

	auto ta = lv::Textarea::create(p)
			  .one_line(true)
			  .placeholder("Type here...")
			  .max_length(40)
			  .width(LV_PCT(100));

	lv::Keyboard::create(p).size(LV_PCT(100), 140).attach(ta);
}

static void page_chart(lv::ObjectView p)
{
	g_chart = lv::Chart::create(p)
			  .size(LV_PCT(95), LV_PCT(85))
			  .type(LV_CHART_TYPE_LINE)
			  .point_count(60)
			  .range(LV_CHART_AXIS_PRIMARY_Y, 0, 100)
			  .update_mode(LV_CHART_UPDATE_MODE_SHIFT)
			  .div_line_count(5, 6)
			  .center();

	g_series = g_chart.add_series(lv_palette_main(LV_PALETTE_CYAN), LV_CHART_AXIS_PRIMARY_Y);
}

static void page_table(lv::ObjectView p)
{
	auto table = lv::Table::create(p).column_count(2).row_count(4);
	table.column_width(0, 120).column_width(1, 120);
	table.cell_value(0, 0, "Key").cell_value(0, 1, "Value");
	table.cell_value(1, 0, "Language").cell_value(1, 1, "C++");
	table.cell_value(2, 0, "LVGL").cell_value(2, 1, "9.2");
	table.cell_value(3, 0, "RTOS").cell_value(3, 1, "oveRTOS");
	table.center();
}

static void page_list(lv::ObjectView p)
{
	auto list = lv::List::create(p).size(240, 160);
	list.add_text("Navigation");
	list.add_button(nullptr, "Settings");
	list.add_button(nullptr, "About");
	list.add_button(nullptr, "Help");
	list.add_button(nullptr, "Quit");
	list.center();
}

static void page_image(lv::ObjectView p)
{
	lv::Image::create(p).src(&badge).center();
}

static void page_canvas(lv::ObjectView p)
{
	auto canvas = lv::Canvas::create(p).size(64, 64);
	canvas.buffer(g_canvas_buf, 64, 64, LV_COLOR_FORMAT_XRGB8888);
	canvas.fill_bg(lv_color_hex(0x202020), LV_OPA_COVER);
	for (int32_t y = 0; y < 64; y++)
		for (int32_t x = 0; x < 64; x++)
			canvas.set_pixel(x, y, lv_color_make(uint8_t(x * 4), uint8_t(y * 4), 128));
	canvas.center();
}

static void page_calendar(lv::ObjectView p)
{
	lv::Calendar::create(p).size(240, 240).today(2026, 4, 13).showed(2026, 4).center();
}

static void page_msgbox(lv::ObjectView p)
{
	auto btn = lv::Button::create(p).size(200, 48);
	lv::Label::create(btn).text("Show Msgbox").center();
	btn.on_click(+[](lv_event_t *) {
		auto mbox = lv::Msgbox::create(lv::ObjectView());
		mbox.add_title("Hello").add_text("Message box from the gallery.").add_close_button();
		lv_obj_center(mbox.get());
	});
	btn.center();
}

static void page_box(lv::ObjectView p)
{
	lv::Box::create(p)
		.size(200, 120)
		.bg_color(lv_color_hex(0x1A237E))
		.bg_opa(LV_OPA_COVER)
		.border_color(lv_color_white())
		.border_width(2)
		.radius(16)
		.pad_all(16)
		.center();
	/* Just a styled container — demonstrates Box API. */
}

static void page_tabview(lv::ObjectView p)
{
	auto tv = lv::Tabview::create(p);
	tv.size(LV_PCT(90), LV_PCT(80)).center().tab_bar_size(32);

	auto t1 = tv.add_tab("Tab A");
	lv::Label::create(t1).text("Content A").center();

	auto t2 = tv.add_tab("Tab B");
	lv::Label::create(t2).text("Content B").center();
}

/* ── Page directory ───────────────────────────────────────────────── */

struct Page {
	const char *name;
	void (*build)(lv::ObjectView);
};

static const Page pages[] = {
	{"Label", page_label},	     {"Button", page_button},	{"Switch", page_switch},
	{"Checkbox", page_checkbox}, {"Bar", page_bar},		{"Slider", page_slider},
	{"Arc", page_arc},	     {"Spinner", page_spinner}, {"Led", page_led},
	{"Dropdown", page_dropdown}, {"Roller", page_roller},	{"Spinbox", page_spinbox},
	{"Text+Kbd", page_textarea}, {"Chart", page_chart},	{"Table", page_table},
	{"List", page_list},	     {"Image", page_image},	{"Canvas", page_canvas},
	{"Calendar", page_calendar}, {"Msgbox", page_msgbox},	{"Box", page_box},
	{"Tabview", page_tabview},
};

static constexpr int N_PAGES = static_cast<int>(sizeof(pages) / sizeof(pages[0]));

/* ── Navigation ───────────────────────────────────────────────────── */

static void clear_live_widgets()
{
	g_bar = lv::Bar{nullptr};
	g_arc = lv::Arc{nullptr};
	g_led = lv::Led{nullptr};
	g_chart = lv::Chart{nullptr};
	g_series = lv::Chart::Series{};
}

static void rebuild_page()
{
	clear_live_widgets();
	g_content.clean();

	pages[g_page].build(g_content);

	etl::string<48> title;
	etl::string_stream ss(title);
	ss << pages[g_page].name << " (" << (g_page + 1) << "/" << N_PAGES << ")";
	g_title_label.text(title.c_str());
}

static void on_prev(lv_event_t *)
{
	g_page = (g_page + N_PAGES - 1) % N_PAGES;
	rebuild_page();
}

static void on_next(lv_event_t *)
{
	g_page = (g_page + 1) % N_PAGES;
	rebuild_page();
}

/* ── Timer ────────────────────────────────────────────────────────── */

static void tick_cb(lv_timer_t *)
{
	static int32_t tick = 0;
	tick++;

	if (g_bar)
		g_bar.value(tick % 101, LV_ANIM_OFF);
	if (g_arc)
		g_arc.value(tick % 101);
	if (g_led && (tick % 10) == 0)
		g_led.toggle();
	if (g_chart && g_series) {
		int32_t v = (tick * 3) % 100;
		g_series.next_value(v);
	}
#if LV_USE_OBSERVER
	g_counter.set(tick);
#endif
}

/* ── UI construction ──────────────────────────────────────────────── */

static void create_ui()
{
	auto screen = lv::ObjectView::screen_active();
	lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_gap(screen, 0, LV_PART_MAIN);

	/* ── Top nav bar ─────────────────────────────────────────────── */
	auto nav = lv::Box::create(screen)
			   .size(LV_PCT(100), 40)
			   .bg_color(lv_color_hex(0x1A237E))
			   .bg_opa(LV_OPA_COVER)
			   .radius(0)
			   .flex_flow(LV_FLEX_FLOW_ROW)
			   .flex_align(LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
				       LV_FLEX_ALIGN_CENTER)
			   .pad_hor(4);

	/* Left arrow */
	auto prev_btn = lv::Button::create(nav).size(40, 32);
	lv::Label::create(prev_btn).text("<").center();
	prev_btn.on_click(&on_prev);

	/* Title */
	g_title_label = lv::Label::create(nav)
				.text("")
				.color(lv_color_white())
				.font(&lv_font_montserrat_14)
				.flex_grow(1)
				.text_align(LV_TEXT_ALIGN_CENTER);

	/* Right arrow */
	auto next_btn = lv::Button::create(nav).size(40, 32);
	lv::Label::create(next_btn).text(">").center();
	next_btn.on_click(&on_next);

	/* ── Content container ─────────────────────────────────────── */
	g_content = lv::Box::create(screen)
			    .size(LV_PCT(100), LV_SIZE_CONTENT)
			    .flex_grow(1)
			    .bg_opa(LV_OPA_TRANSP)
			    .border_width(0)
			    .pad_all(8);

	/* Default group for keyboard nav */
	static lv::Group nav_group;
	nav_group = lv::Group::create();
	nav_group.set_as_default();

	/* Timer for live widgets */
	static lv::Timer ui_timer(tick_cb, 100);
	(void)ui_timer;

	/* Show first page */
	rebuild_page();
}

/* ── Graphics thread ──────────────────────────────────────────────── */

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

		ove::Thread<>::sleep_ms(30);
	}
}

/* ── Application entry point ──────────────────────────────────────── */

OVE_MAIN()
{
	OVE_LOG_INF("LVGL gallery (C++ heap mode): init");

	auto gfx_thread = std::make_unique<ove::Thread<4096>>(graphics_thread, nullptr,
							      OVE_PRIO_HIGH, "graphics");
	(void)gfx_thread;

	int ret = ove_lvgl_init();
	if (ret != OVE_OK) {
		OVE_LOG_ERR("ove_lvgl_init failed: %d", ret);
		return;
	}

	{
		lv::LvglGuard guard;
		create_ui();
	}

	OVE_LOG_INF("LVGL gallery (C++): ready");
	ove::run();
}
