/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS LVGL Rendering Benchmark (C++)
 *
 * Reimplements LVGL's benchmark demo scenes using the C++ binding API
 * through oveRTOS. 16 rendering scenes stress-test widgets, animations,
 * layout, images, and compositing. A summary table shows per-scene
 * FPS / CPU / render / flush metrics.
 */

#include <ove/ove.hpp>
#include <ove/lvgl.hpp>
#include <cstdio>
#include <memory>

extern "C" {
#include "benchmark_perf.h"

LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_rgb);
LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_argb);
LV_IMAGE_DECLARE(img_benchmark_avatar);
}

namespace lv = ove::lvgl;

/*
 * perf_ffi — C-coupled callback and helper boundary.
 *
 * Every entity that crosses into C-ABI land (LVGL animation exec
 * callbacks, `benchmark_*` perf helpers, the sysmon observer, the
 * table draw-task event callback) lives in this namespace so the
 * audit boundary between "safe C++ scene code" and "C-linkage glue"
 * is visible at a glance. The scene callbacks themselves remain in
 * the file's top-level scope and reference `perf_ffi::*` as needed.
 */
namespace perf_ffi
{
// Forward-declared here; definitions live further down so the scene
// callbacks that invoke them see declarations.
void color_anim(lv_obj_t *obj);
void shake_anim(lv_obj_t *obj, int32_t y_max);
void scroll_anim(lv_obj_t *obj, int32_t y_max);
void arc_anim(lv_obj_t *obj);

extern "C" void color_anim_cb(void *var, int32_t v);
extern "C" void shake_anim_y_cb(void *var, int32_t v);
extern "C" void scroll_anim_y_cb(void *var, int32_t v);
extern "C" void arc_anim_cb(void *var, int32_t v);
extern "C" void slideshow_scroll_cb(void *var, int32_t v);
extern "C" void slideshow_ready_cb(lv_anim_t *a);
extern "C" void gauge_arc_exec_cb(void *var, int32_t v);
extern "C" void table_draw_task_event_cb(lv_event_t *e);
#if LV_USE_PERF_MONITOR
extern "C" void sysmon_perf_observer_cb(lv_observer_t *observer, lv_subject_t *subject);
#endif
} /* namespace perf_ffi */

/* ── Forward declarations ─────────────────────────────────────────── */

static void graphics_thread(void *arg);

static void load_scene(uint32_t scene);
static void next_scene_timer_cb(lv_timer_t *timer);
static void summary_create(void);

static void rnd_reset(void);
static int32_t rnd_next(int32_t min, int32_t max);

static lv_obj_t *card_create(void);
static void widgets_demo_cb(void);
// All animation / perf / observer callbacks live in `perf_ffi` — see top of file.

/* Heap-mode graphics thread is allocated inside OVE_MAIN(). */

/* ── Scene types ──────────────────────────────────────────────────── */

struct scene_dsc_t {
	const char *name;
	void (*create_cb)(void);
	uint32_t scene_time;
	uint32_t cpu_avg_usage;
	uint32_t fps_avg;
	uint32_t render_avg_time;
	uint32_t flush_avg_time;
	uint32_t measurement_cnt;
};

/* ── Scene callbacks ──────────────────────────────────────────────── */

static void empty_screen_cb(void)
{
	perf_ffi::color_anim(lv_screen_active());
}

static void moving_wallpaper_cb(void)
{
	lv::Screen::active().pad_all(0);

	auto img = lv::Image::create(lv::ObjectView(lv_screen_active()));
	img.size(lv_pct(150), lv_pct(150));
	lv_image_set_src(img.get(), &img_benchmark_lvgl_logo_rgb);
	lv_image_set_inner_align(img.get(), LV_IMAGE_ALIGN_TILE);
	perf_ffi::shake_anim(img.get(), -lv::display_height() / 3);
}

static void single_rectangle_cb(void)
{
	auto obj = lv::Box::create(lv::Screen::active());
	obj.remove_style_all();
	obj.bg_opa(LV_OPA_COVER);
	obj.center();
	obj.size(lv_pct(30), lv_pct(30));
	perf_ffi::color_anim(obj.get());
}

static void multiple_rectangles_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
		       LV_FLEX_ALIGN_SPACE_EVENLY);

	for (uint32_t i = 0; i < 9; i++) {
		auto obj = lv::Box::create(scr);
		obj.remove_style_all();
		obj.bg_opa(LV_OPA_COVER);
		obj.size(lv_pct(25), lv_pct(25));
		perf_ffi::color_anim(obj);
	}
}

static void multiple_rgb_images_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	scr.pad_row(20);

	int32_t hor = (static_cast<int32_t>(lv::display_width()) - 16) / 116;
	int32_t ver = (static_cast<int32_t>(lv::display_height()) - 116) / 116;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			auto img = lv::Image::create(lv::ObjectView(scr));
			img.src(&img_benchmark_lvgl_logo_rgb);
			if (x == 0)
				img.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			perf_ffi::shake_anim(img.get(), 80);
		}
	}
}

static void multiple_argb_images_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	scr.pad_row(20);

	int32_t hor = (static_cast<int32_t>(lv::display_width()) - 16) / 116;
	int32_t ver = (static_cast<int32_t>(lv::display_height()) - 116) / 116;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			auto img = lv::Image::create(lv::ObjectView(scr));
			img.src(&img_benchmark_lvgl_logo_argb);
			if (x == 0)
				img.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			perf_ffi::shake_anim(img.get(), 80);
		}
	}
}

static void rotated_argb_images_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	scr.pad_row(20);

	int32_t hor = (static_cast<int32_t>(lv::display_width()) - 16) / 116;
	int32_t ver = (static_cast<int32_t>(lv::display_height()) - 116) / 116;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			auto img = lv::Image::create(lv::ObjectView(scr));
			img.src(&img_benchmark_lvgl_logo_argb);
			if (x == 0)
				img.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			img.rotation(rnd_next(100, 3500));
			perf_ffi::shake_anim(img.get(), 80);
		}
	}
}

static void multiple_labels_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	scr.pad_row(80);

	lv_point_t s;
	lv_text_get_size(&s, "Hello LVGL!", lv_obj_get_style_text_font(scr, LV_PART_MAIN), 0, 0,
			 LV_COORD_MAX, LV_TEXT_FLAG_NONE);

	int32_t cnt = (lv::display_width() - 16) / (s.x + 30);
	cnt *= ((lv::display_height() - 200) / (s.y + 50));
	if (cnt < 1)
		cnt = 1;

	for (int32_t i = 0; i < cnt; i++) {
		auto lbl = lv::Label::create(lv::ObjectView(scr));
		lbl.text("Hello LVGL!");
		perf_ffi::color_anim(lbl.get());
	}
}

static void screen_sized_text_cb(void)
{
	static const char *txt = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
				 "Nulla nec rhoncus arcu, in consectetur orci. Sed vitae dolor "
				 "sed nisi ultrices vehicula quis ac dolor. Vivamus hendrerit "
				 "hendrerit lectus, sed tempus velit suscipit in. Fusce eu "
				 "tristique arcu. Sed et molestie leo, in lacinia nunc. Quisque "
				 "semper lorem sed ante feugiat, at molestie risus blandit. "
				 "Maecenas lobortis urna in diam feugiat porta. Ut facilisis "
				 "mauris eget nibh posuere aliquet. Proin facilisis egestas "
				 "magna, id vulputate massa bibendum a.\n\n"
				 "Phasellus iaculis malesuada molestie. Cras ullamcorper justo "
				 "a dolor dignissim tincidunt. Mauris euismod risus quis "
				 "lobortis mollis. Ut vitae placerat massa, aliquet various "
				 "lectus. Nulla ac ornare purus, quis auctor velit. Donec "
				 "posuere dolor rhoncus efficitur dictum. Integer venenatis "
				 "aliquet nunc eu convallis. Nunc quis various velit. "
				 "Suspendisse enim metus, molestie eget mauris sit amet, "
				 "euismod volutpat turpis.\n\n"
				 "Aliquam id tellus in enim hendrerit mattis. Sed ipsum arcu, "
				 "feugiat sed eros quis, vulputate facilisis turpis. Quisque "
				 "venenatis risus massa. Proin lacinia, nunc non ultrices "
				 "commodo, ligula dolor lobortis lectus, iaculis pulvinar metus "
				 "orci eu elit. Donec tincidunt lacinia semper. Class aptent "
				 "taciti sociosqu ad litora torquent per conubia nostra, per "
				 "inceptos himenaeos.\n\n"
				 "Integer vehicula vestibulum eros. Donec facilisis magna a est "
				 "cursus, sed posuere velit faucibus. In et ultrices lorem. Sed "
				 "et lacus finibus, vulputate odio et, finibus tellus. Aenean "
				 "finibus nibh vehicula elementum maximus.\n\n"
				 "Fusce dignissim turpis massa, eget semper purus semper at. "
				 "Ut et augue vitae metus laoreet auctor. Morbi tincidunt, "
				 "neque vel tincidunt interdum, sapien nibh finibus lorem, eu "
				 "eleifend diam ipsum et eros.";

	auto scr = lv::Screen::active();
	auto lbl = lv::Label::create(lv::ObjectView(scr));
	lbl.width(lv_pct(100));
	lbl.text(txt);
	lv_obj_update_layout(lbl.get());
	perf_ffi::scroll_anim(scr, scr.get_scroll_bottom());
}

static void multiple_arcs_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	int32_t hor = (lv::display_width() - 16) / lv_dpx(160);
	int32_t ver = (lv::display_height() - 16) / lv_dpx(160);
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			auto a = lv::Arc::create(lv::ObjectView(scr));
			if (x == 0)
				a.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			a.size(lv_dpx(100), lv_dpx(100));
			a.center();
			a.bg_angles(0, 360);
			a.margin_top(lv_dpx(20))
				.margin_bottom(lv_dpx(20))
				.margin_left(lv_dpx(20))
				.margin_right(lv_dpx(20));
			a.arc_opa(0, LV_PART_MAIN);
			a.bg_opa(0, LV_PART_KNOB);
			a.arc_width(10, LV_PART_INDICATOR);
			a.arc_rounded(false, LV_PART_INDICATOR);
			a.arc_color(lv_color_hex3(rnd_next(0x00f, 0xff0)), LV_PART_INDICATOR);
			perf_ffi::arc_anim(a.get());
		}
	}
}

static void containers_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	int32_t hor = (static_cast<int32_t>(lv::display_width()) - 16) / 300;
	int32_t ver = (static_cast<int32_t>(lv::display_height()) - 16) / 150;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *card = card_create();
			if (x == 0)
				lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			perf_ffi::shake_anim(card, 30);
		}
	}
}

static void containers_with_overlay_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	int32_t hor = (static_cast<int32_t>(lv::display_width()) - 16) / 300;
	int32_t ver = (static_cast<int32_t>(lv::display_height()) - 16) / 150;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *card = card_create();
			if (x == 0)
				lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			perf_ffi::shake_anim(card, 30);
		}
	}

	lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_50, 0);
	perf_ffi::color_anim(lv_layer_top());
}

static void containers_with_opa_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	int32_t hor = (static_cast<int32_t>(lv::display_width()) - 16) / 300;
	int32_t ver = (static_cast<int32_t>(lv::display_height()) - 16) / 150;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *card = card_create();
			if (x == 0)
				lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			lv_obj_set_style_opa(card, LV_OPA_50, 0);
			perf_ffi::shake_anim(card, 30);
		}
	}
}

static void containers_with_opa_layer_cb(void)
{
	auto scr = lv::Screen::active();
	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	int32_t hor = (static_cast<int32_t>(lv::display_width()) - 16) / 300;
	int32_t ver = (static_cast<int32_t>(lv::display_height()) - 16) / 150;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *card = card_create();
			lv_obj_set_style_opa_layered(card, LV_OPA_50, 0);
			if (x == 0)
				lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			perf_ffi::shake_anim(card, 30);
		}
	}
}

static void containers_with_scrolling_cb(void)
{
	auto scr = lv::Screen::active();

	scr.flex_flow(LV_FLEX_FLOW_ROW_WRAP);
	scr.flex_align(LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	for (uint32_t i = 0; i < 50; i++)
		card_create();

	scr.update_layout();
	perf_ffi::scroll_anim(scr, scr.get_scroll_bottom());
}

/* ── Widgets demo scene ────────────────────────────────────────────── */

static lv_obj_t *g_tabview;
static uint32_t g_slideshow_tab;

namespace perf_ffi
{

extern "C" void slideshow_scroll_cb(void *var, int32_t v)
{
	lv_obj_scroll_to_y(static_cast<lv_obj_t *>(var), v, LV_ANIM_OFF);
}

extern "C" void slideshow_ready_cb(lv_anim_t *a)
{
	(void)a;
	if (!g_tabview)
		return;

	g_slideshow_tab = (g_slideshow_tab + 1) % 3;
	lv_tabview_set_active(g_tabview, g_slideshow_tab, LV_ANIM_ON);

	lv_obj_t *tab = lv_tabview_get_content(g_tabview);
	if (!tab)
		return;
	/* Get the active tab page (child at index = active tab) */
	tab = lv_obj_get_child(tab, static_cast<int32_t>(g_slideshow_tab));
	if (!tab)
		return;

	lv_obj_update_layout(tab);
	int32_t bot = lv_obj_get_scroll_bottom(tab);
	if (bot <= 0)
		bot = 1;

	uint32_t spd = lv_anim_speed(lv::display_dpi());
	benchmark_anim_slideshow(tab, slideshow_scroll_cb, bot, spd, slideshow_ready_cb);
}

extern "C" void gauge_arc_exec_cb(void *var, int32_t v)
{
	lv_arc_set_value(static_cast<lv_obj_t *>(var), v);
}

} /* namespace perf_ffi */

static void widgets_demo_cb(void)
{
	auto scr = lv::Screen::active();
	scr.pad_all(0);
	lv_obj_set_style_pad_top(scr, 0, 0);

	/* ── Tabview ─────────────────────────────────── */

	auto tabview = lv::Tabview::create(lv::ObjectView(scr));
	tabview.tab_bar_size(40);
	g_tabview = tabview.get();

	auto tab1 = tabview.add_tab("Form");
	auto tab2 = tabview.add_tab("Gauges");
	auto tab3 = tabview.add_tab("Pickers");

	/* ── Tab 1: Form widgets ─────────────────────── */

	lv_obj_set_flex_flow(tab1, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_gap(tab1, 10, 0);

	auto ta = lv::Textarea::create(tab1);
	ta.one_line(true).placeholder("Username").width(lv_pct(90));

	auto dd = lv::Dropdown::create(tab1);
	dd.options("Option A\nOption B\nOption C").width(lv_pct(90));

	auto slider = lv::Slider::create(tab1);
	slider.value(40, LV_ANIM_OFF).width(lv_pct(90));

	auto sw = lv::Switch::create(tab1);
	sw.checked(true);

	auto cb = lv::Checkbox::create(tab1);
	cb.text("I agree");

	auto btn = lv::Button::create(tab1);
	btn.width(lv_pct(90));
	auto btn_label = lv::Label::create(btn);
	btn_label.text("Submit").center();

	/* ── Tab 2: Gauges (Scale + Arc animations) ──── */

	lv_obj_set_flex_flow(tab2, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(tab2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_gap(tab2, 10, 0);

	/* Gauge 1: circular 360 deg with 3 concentric arcs */
	{
		lv_obj_t *gauge_box = lv_obj_create(tab2);
		lv_obj_set_size(gauge_box, 200, 200);
		lv_obj_set_style_pad_all(gauge_box, 0, 0);
		lv_obj_set_style_border_width(gauge_box, 0, 0);
		lv_obj_set_style_bg_opa(gauge_box, LV_OPA_TRANSP, 0);

		lv_obj_t *scale = lv_scale_create(gauge_box);
		lv_obj_set_size(scale, 180, 180);
		lv_obj_center(scale);
		lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_OUTER);
		lv_scale_set_range(scale, 0, 100);
		lv_scale_set_total_tick_count(scale, 11);
		lv_scale_set_major_tick_every(scale, 5);
		lv_scale_set_angle_range(scale, 360);

		/* 3 animated arcs at different rates */
		static const struct {
			uint32_t t1;
			uint32_t t2;
			lv_palette_t pal;
			int32_t margin;
		} arcs[] = {
			{4100, 2700, LV_PALETTE_BLUE, 0},
			{2600, 3200, LV_PALETTE_RED, 20},
			{2800, 1800, LV_PALETTE_GREEN, 40},
		};
		for (int i = 0; i < 3; i++) {
			auto arc = lv::Arc::create(lv::ObjectView(gauge_box));
			arc.size(180 - arcs[i].margin * 2, 180 - arcs[i].margin * 2);
			arc.center();
			arc.range(0, 100);
			arc.bg_angles(0, 360);
			lv_obj_set_style_arc_opa(arc.get(), 0, LV_PART_MAIN);
			lv_obj_set_style_bg_opa(arc.get(), 0, LV_PART_KNOB);
			arc.indicator_width(8);
			arc.indicator_color(lv_palette_main(arcs[i].pal));

			lv_anim_t a;
			lv_anim_init(&a);
			lv_anim_set_var(&a, arc.get());
			lv_anim_set_exec_cb(&a, perf_ffi::gauge_arc_exec_cb);
			lv_anim_set_values(&a, 20, 100);
			lv_anim_set_duration(&a, arcs[i].t1);
			lv_anim_set_playback_duration(&a, arcs[i].t2);
			lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
			lv_anim_start(&a);
		}
	}

	/* Gauge 2: semi-circular 270 deg with sections */
	{
		lv_obj_t *gauge_box = lv_obj_create(tab2);
		lv_obj_set_size(gauge_box, 200, 200);
		lv_obj_set_style_pad_all(gauge_box, 0, 0);
		lv_obj_set_style_border_width(gauge_box, 0, 0);
		lv_obj_set_style_bg_opa(gauge_box, LV_OPA_TRANSP, 0);

		lv_obj_t *scale = lv_scale_create(gauge_box);
		lv_obj_set_size(scale, 180, 180);
		lv_obj_center(scale);
		lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_OUTER);
		lv_scale_set_range(scale, 10, 60);
		lv_scale_set_total_tick_count(scale, 21);
		lv_scale_set_major_tick_every(scale, 4);
		lv_scale_set_angle_range(scale, 270);
		lv_scale_set_rotation(scale, 135);

		/* Colored sections */
		static lv_style_t style_red, style_blue, style_green;
		lv_style_init(&style_red);
		lv_style_set_arc_color(&style_red, lv_palette_main(LV_PALETTE_RED));
		lv_style_init(&style_blue);
		lv_style_set_arc_color(&style_blue, lv_palette_main(LV_PALETTE_BLUE));
		lv_style_init(&style_green);
		lv_style_set_arc_color(&style_green, lv_palette_main(LV_PALETTE_GREEN));

		lv_scale_section_t *sec;
		sec = lv_scale_add_section(scale);
		lv_scale_section_set_range(sec, 10, 25);
		lv_scale_section_set_style(sec, LV_PART_INDICATOR, &style_red);
		sec = lv_scale_add_section(scale);
		lv_scale_section_set_range(sec, 25, 45);
		lv_scale_section_set_style(sec, LV_PART_INDICATOR, &style_blue);
		sec = lv_scale_add_section(scale);
		lv_scale_section_set_range(sec, 45, 60);
		lv_scale_section_set_style(sec, LV_PART_INDICATOR, &style_green);

		/* Animated indicator arc */
		auto arc = lv::Arc::create(lv::ObjectView(gauge_box));
		arc.size(160, 160);
		arc.center();
		arc.range(10, 60);
		arc.bg_angles(0, 270);
		arc.rotation(135);
		lv_obj_set_style_arc_opa(arc.get(), 0, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(arc.get(), 0, LV_PART_KNOB);
		arc.indicator_width(12);

		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, arc.get());
		lv_anim_set_exec_cb(&a, perf_ffi::gauge_arc_exec_cb);
		lv_anim_set_values(&a, 10, 60);
		lv_anim_set_duration(&a, 4100);
		lv_anim_set_playback_duration(&a, 800);
		lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
		lv_anim_start(&a);
	}

	/* Line chart: 12 points */
	{
		auto chart = lv::Chart::create(tab2);
		chart.size(200, 140);
		chart.type(LV_CHART_TYPE_LINE);
		chart.point_count(12);
		auto ser =
			chart.add_series(lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
		static const int32_t data[] = {10, 20, 30, 25, 40, 35, 50, 60, 55, 70, 65, 80};
		for (int i = 0; i < 12; i++)
			ser.set_value_by_idx(i, data[i]);
	}

	/* ── Tab 3: Pickers ──────────────────────────── */

	lv_obj_set_flex_flow(tab3, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(tab3, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_gap(tab3, 10, 0);

	auto cal = lv::Calendar::create(tab3);
	cal.size(200, 200);
	cal.today(2026, 4, 13);
	cal.showed(2026, 4);

	auto roller = lv::Roller::create(tab3);
	roller.options("Mon\nTue\nWed\nThu\nFri\nSat\nSun", LV_ROLLER_MODE_NORMAL);
	roller.visible_row_count(3);

	auto spinbox = lv::Spinbox::create(tab3);
	spinbox.range(0, 100);
	spinbox.value(42);
	spinbox.step(1);

	/* Bar chart: 7 points */
	{
		auto chart = lv::Chart::create(tab3);
		chart.size(200, 140);
		chart.type(LV_CHART_TYPE_BAR);
		chart.point_count(7);
		auto ser = chart.add_series(lv_palette_main(LV_PALETTE_GREEN),
					    LV_CHART_AXIS_PRIMARY_Y);
		static const int32_t data[] = {40, 55, 30, 70, 50, 65, 45};
		for (int i = 0; i < 7; i++)
			ser.set_value_by_idx(i, data[i]);
	}

	/* ── Start slideshow ─────────────────────────── */

	g_slideshow_tab = 0;

	lv_obj_update_layout(tab1);
	int32_t bot = lv_obj_get_scroll_bottom(tab1);
	if (bot <= 0)
		bot = 1;

	uint32_t spd = lv_anim_speed(lv::display_dpi());
	benchmark_anim_slideshow(tab1, perf_ffi::slideshow_scroll_cb, bot, spd,
				 perf_ffi::slideshow_ready_cb);
}

/* ── Scene array ──────────────────────────────────────────────────── */

static scene_dsc_t scenes[] = {
	{"Empty screen", empty_screen_cb, 3000, 0, 0, 0, 0, 0},
	{"Moving wallpaper", moving_wallpaper_cb, 3000, 0, 0, 0, 0, 0},
	{"Single rectangle", single_rectangle_cb, 3000, 0, 0, 0, 0, 0},
	{"Multiple rectangles", multiple_rectangles_cb, 3000, 0, 0, 0, 0, 0},
	{"Multiple RGB images", multiple_rgb_images_cb, 3000, 0, 0, 0, 0, 0},
	{"Multiple ARGB images", multiple_argb_images_cb, 3000, 0, 0, 0, 0, 0},
	{"Rotated ARGB images", rotated_argb_images_cb, 3000, 0, 0, 0, 0, 0},
	{"Multiple labels", multiple_labels_cb, 3000, 0, 0, 0, 0, 0},
	{"Screen sized text", screen_sized_text_cb, 5000, 0, 0, 0, 0, 0},
	{"Multiple arcs", multiple_arcs_cb, 3000, 0, 0, 0, 0, 0},
	{"Containers", containers_cb, 3000, 0, 0, 0, 0, 0},
	{"Containers with overlay", containers_with_overlay_cb, 3000, 0, 0, 0, 0, 0},
	{"Containers with opa", containers_with_opa_cb, 3000, 0, 0, 0, 0, 0},
	{"Containers with opa_layer", containers_with_opa_layer_cb, 3000, 0, 0, 0, 0, 0},
	{"Containers with scrolling", containers_with_scrolling_cb, 5000, 0, 0, 0, 0, 0},
	{"Widgets demo", widgets_demo_cb, 20000, 0, 0, 0, 0, 0},
	{"", nullptr, 0, 0, 0, 0, 0, 0},
};

static uint32_t scene_act;
static uint32_t rnd_act;

/* ── Scene management ─────────────────────────────────────────────── */

static void load_scene(uint32_t scene)
{
	auto scr = lv::Screen::active();
	scr.clean();
	lv_obj_set_style_bg_color(scr, lv_palette_lighten(LV_PALETTE_GREY, 4), 0);
	lv_obj_set_style_text_color(scr, lv_color_black(), 0);
	scr.pad_all(8);
	lv_obj_set_style_pad_top(scr, 40, 0);
	lv_obj_set_style_pad_gap(scr, 8, 0);
	scr.layout(LV_LAYOUT_NONE);
	scr.flex_align(LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_anim_delete(scr, perf_ffi::scroll_anim_y_cb);
	lv_anim_delete(scr, perf_ffi::shake_anim_y_cb);
	lv_anim_delete(scr, perf_ffi::color_anim_cb);

	lv_anim_delete(lv_layer_top(), perf_ffi::color_anim_cb);
	lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);

	rnd_reset();
	if (scenes[scene].create_cb)
		scenes[scene].create_cb();
}

static void next_scene_timer_cb(lv_timer_t *timer)
{
	scene_act++;
	load_scene(scene_act);

	if (scenes[scene_act].scene_time == 0) {
		lv_timer_delete(timer);
		summary_create();
	} else {
		lv_timer_set_period(timer, scenes[scene_act].scene_time);
	}
}

/* ── Performance observer ─────────────────────────────────────────── */

#if LV_USE_PERF_MONITOR
namespace perf_ffi
{
extern "C" void sysmon_perf_observer_cb(lv_observer_t *observer, lv_subject_t *subject)
{
	benchmark_perf_metrics_t m;
	benchmark_extract_perf_metrics(lv_subject_get_pointer(subject), &m);
	lv_obj_t *label = static_cast<lv_obj_t *>(lv_observer_get_target(observer));

	char scene_name[64];
	if (scenes[scene_act].name[0] != '\0')
		snprintf(scene_name, sizeof(scene_name), "%s: ", scenes[scene_act].name);
	else
		scene_name[0] = '\0';

	lv_label_set_text_fmt(label,
			      "%s%u FPS, %u%% CPU\n"
			      "refr. %u ms = %u ms render + %u ms flush",
			      scene_name, static_cast<unsigned>(m.fps),
			      static_cast<unsigned>(m.cpu),
			      static_cast<unsigned>(m.render_avg_time + m.flush_avg_time),
			      static_cast<unsigned>(m.render_avg_time),
			      static_cast<unsigned>(m.flush_avg_time));

	/* Ignore first call — stale data from previous scene */
	if (scenes[scene_act].measurement_cnt != 0) {
		scenes[scene_act].cpu_avg_usage += m.cpu;
		scenes[scene_act].fps_avg += m.fps;
		scenes[scene_act].render_avg_time += m.render_avg_time;
		scenes[scene_act].flush_avg_time += m.flush_avg_time;
	}
	scenes[scene_act].measurement_cnt++;
}
} /* namespace perf_ffi */
#endif

/* ── Summary table ────────────────────────────────────────────────── */

namespace perf_ffi
{
extern "C" void table_draw_task_event_cb(lv_event_t *e)
{
	lv_draw_task_t *t = lv_event_get_draw_task(e);
	lv_draw_dsc_base_t *base = static_cast<lv_draw_dsc_base_t *>(lv_draw_task_get_draw_dsc(t));
	if (base->part != LV_PART_ITEMS)
		return;

	int32_t row = base->id1;
	if (row == 0) {
		lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(t);
		if (fill)
			fill->color = lv_palette_darken(LV_PALETTE_BLUE_GREY, 4);
		lv_draw_label_dsc_t *lbl = lv_draw_task_get_label_dsc(t);
		if (lbl)
			lbl->color = lv_color_white();
	} else if (row == 1) {
		lv_draw_border_dsc_t *border = lv_draw_task_get_border_dsc(t);
		if (border) {
			border->color = lv_palette_darken(LV_PALETTE_BLUE_GREY, 4);
			border->width = 2;
			border->side = LV_BORDER_SIDE_BOTTOM;
		}
		lv_draw_label_dsc_t *lbl = lv_draw_task_get_label_dsc(t);
		if (lbl)
			lbl->color = lv_palette_darken(LV_PALETTE_BLUE_GREY, 4);
	}
}
} /* namespace perf_ffi */

static void summary_create(void)
{
	lv::Screen::active().clean();
	lv::Screen::active().pad_hor(0);

	auto table = lv::Table::create(lv::ObjectView(lv_screen_active()));
	lv_obj_set_width(table.get(), lv_pct(100));
	lv_obj_set_style_max_height(table.get(), lv_pct(100), 0);
	lv_obj_add_flag(table.get(), LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
	lv_obj_set_style_text_font(table.get(), &lv_font_montserrat_14, LV_PART_ITEMS);
	lv_obj_set_style_text_font(table.get(), &lv_font_montserrat_14, LV_PART_MAIN);
	lv_obj_set_style_pad_top(table.get(), 2, LV_PART_ITEMS);
	lv_obj_set_style_pad_bottom(table.get(), 2, LV_PART_ITEMS);
	lv_obj_set_style_pad_left(table.get(), 4, LV_PART_ITEMS);
	lv_obj_set_style_pad_right(table.get(), 4, LV_PART_ITEMS);
	lv_obj_set_style_text_color(table.get(), lv_palette_darken(LV_PALETTE_BLUE_GREY, 2),
				    LV_PART_ITEMS);
	lv_obj_set_style_border_color(table.get(), lv_palette_darken(LV_PALETTE_BLUE_GREY, 2),
				      LV_PART_ITEMS);
	lv_obj_add_event_cb(table.get(), perf_ffi::table_draw_task_event_cb,
			    LV_EVENT_DRAW_TASK_ADDED, NULL);

	table.cell_value(0, 0, "Name");
	table.cell_value(0, 1, "Avg. CPU");
	table.cell_value(0, 2, "Avg. FPS");
	table.cell_value(0, 3, "Avg. time (render + flush)");

	OVE_LOG_INF("Benchmark Summary");
	OVE_LOG_INF("Name, Avg. CPU, Avg. FPS, Avg. time, render, flush");

	lv_obj_update_layout(table.get());
	int32_t col_w = lv_obj_get_content_width(table.get()) / 4;
	for (int c = 0; c < 4; c++)
		table.column_width(c, col_w);

	int32_t total_fps = 0, total_cpu = 0;
	int32_t total_render = 0, total_flush = 0;
	int32_t valid = 0;

	for (uint32_t i = 0; scenes[i].create_cb; i++) {
		table.cell_value(i + 2, 0, scenes[i].name);

		if (scenes[i].measurement_cnt <= 1) {
			table.cell_value(i + 2, 1, "N/A");
			table.cell_value(i + 2, 2, "N/A");
			table.cell_value(i + 2, 3, "N/A");
		} else {
			int32_t cnt = scenes[i].measurement_cnt - 1;
			uint32_t cpu = scenes[i].cpu_avg_usage / cnt;
			uint32_t fps = scenes[i].fps_avg / cnt;
			uint32_t render = scenes[i].render_avg_time / cnt;
			uint32_t flush = scenes[i].flush_avg_time / cnt;

			lv_table_set_cell_value_fmt(table.get(), i + 2, 1, "%u %%",
						    static_cast<unsigned>(cpu));
			lv_table_set_cell_value_fmt(table.get(), i + 2, 2, "%u FPS",
						    static_cast<unsigned>(fps));
			lv_table_set_cell_value_fmt(table.get(), i + 2, 3, "%u ms (%u + %u)",
						    static_cast<unsigned>(render + flush),
						    static_cast<unsigned>(render),
						    static_cast<unsigned>(flush));

			OVE_LOG_INF("%s, %u%%, %u, %u, %u, %u", scenes[i].name,
				    static_cast<unsigned>(cpu), static_cast<unsigned>(fps),
				    static_cast<unsigned>(render + flush),
				    static_cast<unsigned>(render), static_cast<unsigned>(flush));

			valid++;
			total_cpu += cpu;
			total_fps += fps;
			total_render += render;
			total_flush += flush;
		}
	}

	table.cell_value(1, 0, "All scenes avg.");
	if (valid < 1) {
		table.cell_value(1, 1, "N/A");
		table.cell_value(1, 2, "N/A");
		table.cell_value(1, 3, "N/A");
	} else {
		uint32_t avg_cpu = total_cpu / valid;
		uint32_t avg_fps = total_fps / valid;
		uint32_t avg_render = total_render / valid;
		uint32_t avg_flush = total_flush / valid;

		lv_table_set_cell_value_fmt(table.get(), 1, 1, "%u %%",
					    static_cast<unsigned>(avg_cpu));
		lv_table_set_cell_value_fmt(table.get(), 1, 2, "%u FPS",
					    static_cast<unsigned>(avg_fps));
		lv_table_set_cell_value_fmt(table.get(), 1, 3, "%u ms (%u + %u)",
					    static_cast<unsigned>(avg_render + avg_flush),
					    static_cast<unsigned>(avg_render),
					    static_cast<unsigned>(avg_flush));

		OVE_LOG_INF("All avg, %u%%, %u, %u, %u, %u", static_cast<unsigned>(avg_cpu),
			    static_cast<unsigned>(avg_fps),
			    static_cast<unsigned>(avg_render + avg_flush),
			    static_cast<unsigned>(avg_render), static_cast<unsigned>(avg_flush));
	}
}

/* ── Animation helpers — live in perf_ffi for C-ABI boundary ─────── */

namespace perf_ffi
{

extern "C" void color_anim_cb(void *var, int32_t v)
{
	(void)v;
	lv_obj_set_style_bg_color(static_cast<lv_obj_t *>(var),
				  lv_color_hex3(rnd_next(0x00f, 0xff0)), 0);
	lv_obj_set_style_text_color(static_cast<lv_obj_t *>(var),
				    lv_color_hex3(rnd_next(0x00f, 0xff0)), 0);
}

void color_anim(lv_obj_t *obj)
{
	lv::Animation()
		.target(obj)
		.values(0, 100)
		.duration(100)
		.exec_cb(color_anim_cb)
		.repeat_count(LV_ANIM_REPEAT_INFINITE)
		.start();
}

extern "C" void arc_anim_cb(void *var, int32_t v)
{
	lv_arc_set_value(static_cast<lv_obj_t *>(var), v);
}

void arc_anim(lv_obj_t *obj)
{
	uint32_t t1 = rnd_next(1000, 3000);
	uint32_t t2 = rnd_next(1000, 3000);

	lv::Animation()
		.target(obj)
		.values(0, 100)
		.duration(t1)
		.playback_duration(t2)
		.exec_cb(arc_anim_cb)
		.repeat_count(LV_ANIM_REPEAT_INFINITE)
		.start();
}

extern "C" void scroll_anim_y_cb(void *var, int32_t v)
{
	lv_obj_scroll_to_y(static_cast<lv_obj_t *>(var), v, LV_ANIM_OFF);
}

void scroll_anim(lv_obj_t *obj, int32_t y_max)
{
	uint32_t t = lv_anim_speed(lv::display_dpi());

	lv::Animation()
		.target(obj)
		.values(0, y_max)
		.duration(t)
		.playback_duration(t)
		.exec_cb(scroll_anim_y_cb)
		.repeat_count(LV_ANIM_REPEAT_INFINITE)
		.start();
}

extern "C" void shake_anim_y_cb(void *var, int32_t v)
{
	lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), v, 0);
}

void shake_anim(lv_obj_t *obj, int32_t y_max)
{
	uint32_t t1 = rnd_next(300, 3000);
	uint32_t t2 = rnd_next(300, 3000);

	lv::Animation()
		.target(obj)
		.values(0, y_max)
		.duration(t1)
		.playback_duration(t2)
		.exec_cb(shake_anim_y_cb)
		.repeat_count(LV_ANIM_REPEAT_INFINITE)
		.start();
}

} /* namespace perf_ffi */

/* ── Card composite widget ────────────────────────────────────────── */

static lv_obj_t *card_create(void)
{
	auto scr = lv::Screen::active();

	lv_obj_t *panel = lv_obj_create(scr);
	lv_obj_set_size(panel, 270, 120);
	lv_obj_set_style_pad_all(panel, 8, 0);

	auto avatar = lv::Image::create(lv::ObjectView(panel));
	avatar.src(&img_benchmark_avatar);
	avatar.align(LV_ALIGN_LEFT_MID, 0, 0);

	auto name = lv::Label::create(lv::ObjectView(panel));
	name.text("John Smith");
#if LV_FONT_MONTSERRAT_24
	name.font(&lv_font_montserrat_24);
#endif
	lv_obj_set_pos(name.get(), 100, 0);

	auto desc = lv::Label::create(lv::ObjectView(panel));
	desc.text("A DIY enthusiast");
#if LV_FONT_MONTSERRAT_14
	desc.font(&lv_font_montserrat_14);
#endif
	lv_obj_set_pos(desc.get(), 100, 30);

	auto btn = lv::Button::create(lv::ObjectView(panel));
	lv_obj_set_pos(btn.get(), 100, 50);

	lv::Label::create(btn).text("Connect");

	return panel;
}

/* ── PRNG ─────────────────────────────────────────────────────────── */

static void rnd_reset(void)
{
	rnd_act = 0;
}

static int32_t rnd_next(int32_t min, int32_t max)
{
	static const uint32_t rnd_map[] = {
		0xbd13204f, 0x67d8167f, 0x20211c99, 0xb0a7cc05, 0x06d5c703, 0xeafb01a7, 0xd0473b5c,
		0xc999aaa2, 0x86f9d5d9, 0x294bdb29, 0x12a3c207, 0x78914d14, 0x10a30006, 0x6134c7db,
		0x194443af, 0x142d1099, 0x376292d5, 0x20f433c5, 0x074d2a59, 0x4e74c293, 0x072a0810,
		0xdd0f136d, 0x5cca6dbc, 0x623bfdd8, 0xb645eb2f, 0xbe50894a, 0xc9b56717, 0xe0f912c8,
		0x4f6b5e24, 0xfe44b128, 0xe12d57a8, 0x9b15c9cc, 0xab2ae1d3, 0xb4dc5074, 0x67d457c8,
		0x8e46b00c, 0xa29a1871, 0xcee40332, 0x80f93aa1, 0x85286096, 0x09bd6b49, 0x95072088,
		0x2093924b, 0x6a27328f, 0xa796079b, 0xc3b488bc, 0xe29bcce0, 0x07048a4c, 0x7d81bd99,
		0x27aacb30, 0x44fc7a0e, 0xa2382241, 0x8357a17d, 0x97e9c9cc, 0xad10ff52, 0x9923fc5c,
		0x8f2c840a, 0x20356ba2, 0x7997a677, 0x9a7f1800, 0x35c7562b, 0xd901fe51, 0x8f4e053d,
		0xa5b94923,
	};

	if (min == max)
		return min;

	if (min > max) {
		int32_t t = min;
		min = max;
		max = t;
	}

	int32_t d = max - min;
	int32_t r = (rnd_map[rnd_act] % d) + min;

	rnd_act++;
	if (rnd_act >= sizeof(rnd_map) / sizeof(rnd_map[0]))
		rnd_act = 0;

	return r;
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

		ove::this_thread::sleep_ms(33);
	}
}

/* ── Application entry point ──────────────────────────────────────── */

OVE_MAIN()
{
	OVE_LOG_INF("LVGL benchmark (C++ heap mode): init");

	auto gfx_thread = std::make_unique<ove::Thread<4096>>(graphics_thread, nullptr,
							      OVE_PRIO_HIGH, "graphics");
	(void)gfx_thread;

	int ret = ove_lvgl_init();
	if (ret != OVE_OK) {
		OVE_LOG_ERR("ove_lvgl_init failed: %d", ret);
		return;
	}

	ove_lvgl_lock();

	scene_act = 0;

	auto scr = lv::Screen::active();
	lv_obj_remove_style_all(scr);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_color(scr, lv_palette_lighten(LV_PALETTE_GREY, 4), 0);
	scr.pad_all(8);
	lv_obj_set_style_pad_top(scr, 40, 0);
	lv_obj_set_style_pad_gap(scr, 8, 0);

	auto title = lv::Label::create(lv::ObjectView(lv_layer_top()));
	lv_obj_set_style_bg_opa(title.get(), LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(title.get(), lv_color_white(), 0);
	title.color(lv_color_black());
	title.font(&lv_font_montserrat_14);
	title.width(lv_pct(100));

	load_scene(scene_act);

	lv_timer_create(next_scene_timer_cb, scenes[0].scene_time, NULL);

#if LV_USE_PERF_MONITOR
	lv_subject_t *perf_subj = benchmark_get_perf_subject();
	if (perf_subj)
		lv_subject_add_observer_obj(perf_subj, perf_ffi::sysmon_perf_observer_cb,
					    title.get(), NULL);
	else
		title.text("Perf monitor unavailable");
#else
	title.text("LV_USE_PERF_MONITOR is not enabled");
#endif

	ove_lvgl_unlock();

	OVE_LOG_INF("LVGL benchmark (C++): running");
	ove::run();
}
