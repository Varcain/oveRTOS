/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS LVGL Rendering Benchmark (C)
 *
 * Reimplements LVGL's benchmark demo scenes using the raw LVGL C API
 * through oveRTOS. 15 rendering scenes stress-test widgets, animations,
 * layout, images, and compositing. A summary table shows per-scene
 * FPS / CPU / render / flush metrics.
 */

#include "ove/ove.h"
#include "ove/lvgl.h"
#include "benchmark_perf.h"
#include <stdio.h>

/* ── Image declarations ────────────────────────────────────────────── */

LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_rgb);
LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_argb);
LV_IMAGE_DECLARE(img_benchmark_avatar);

/* ── Scene types ───────────────────────────────────────────────────── */

typedef struct {
	const char *name;
	void (*create_cb)(void);
	uint32_t scene_time;
	uint32_t cpu_avg_usage;
	uint32_t fps_avg;
	uint32_t render_avg_time;
	uint32_t flush_avg_time;
	uint32_t measurement_cnt;
} scene_dsc_t;

/* ── Forward declarations ──────────────────────────────────────────── */

static void load_scene(uint32_t scene);
static void next_scene_timer_cb(lv_timer_t *timer);
static void summary_create(void);

static void rnd_reset(void);
static int32_t rnd_next(int32_t min, int32_t max);

static void color_anim_cb(void *var, int32_t v);
static void color_anim(lv_obj_t *obj);
static void shake_anim_y_cb(void *var, int32_t v);
static void shake_anim(lv_obj_t *obj, int32_t y_max);
static void scroll_anim_y_cb(void *var, int32_t v);
static void scroll_anim(lv_obj_t *obj, int32_t y_max);
static void arc_anim_cb(void *var, int32_t v);
static void arc_anim(lv_obj_t *obj);
static lv_obj_t *card_create(void);

#if LV_USE_PERF_MONITOR
static void sysmon_perf_observer_cb(lv_observer_t *observer, lv_subject_t *subject);
#endif

/* ── Scene callbacks ───────────────────────────────────────────────── */

static void empty_screen_cb(void)
{
	color_anim(lv_screen_active());
}

static void moving_wallpaper_cb(void)
{
	lv_obj_set_style_pad_all(lv_screen_active(), 0, 0);

	lv_obj_t *img = lv_image_create(lv_screen_active());
	lv_obj_set_size(img, lv_pct(150), lv_pct(150));
	lv_image_set_src(img, &img_benchmark_lvgl_logo_rgb);
	lv_image_set_inner_align(img, LV_IMAGE_ALIGN_TILE);
	shake_anim(img, -lv_display_get_vertical_resolution(NULL) / 3);
}

static void single_rectangle_cb(void)
{
	lv_obj_t *obj = lv_obj_create(lv_screen_active());
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_center(obj);
	lv_obj_set_size(obj, lv_pct(30), lv_pct(30));
	color_anim(obj);
}

static void multiple_rectangles_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_SPACE_EVENLY);

	for (uint32_t i = 0; i < 9; i++) {
		lv_obj_t *obj = lv_obj_create(lv_screen_active());
		lv_obj_remove_style_all(obj);
		lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
		lv_obj_set_size(obj, lv_pct(25), lv_pct(25));
		color_anim(obj);
	}
}

static void multiple_rgb_images_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(lv_screen_active(), 20, 0);

	int32_t hor = ((int32_t)lv_display_get_horizontal_resolution(NULL) - 16) / 116;
	int32_t ver = ((int32_t)lv_display_get_vertical_resolution(NULL) - 116) / 116;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *obj = lv_image_create(lv_screen_active());
			lv_image_set_src(obj, &img_benchmark_lvgl_logo_rgb);
			if (x == 0)
				lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			shake_anim(obj, 80);
		}
	}
}

static void multiple_argb_images_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(lv_screen_active(), 20, 0);

	int32_t hor = ((int32_t)lv_display_get_horizontal_resolution(NULL) - 16) / 116;
	int32_t ver = ((int32_t)lv_display_get_vertical_resolution(NULL) - 116) / 116;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *obj = lv_image_create(lv_screen_active());
			lv_image_set_src(obj, &img_benchmark_lvgl_logo_argb);
			if (x == 0)
				lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			shake_anim(obj, 80);
		}
	}
}

static void rotated_argb_images_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(lv_screen_active(), 20, 0);

	int32_t hor = ((int32_t)lv_display_get_horizontal_resolution(NULL) - 16) / 116;
	int32_t ver = ((int32_t)lv_display_get_vertical_resolution(NULL) - 116) / 116;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *obj = lv_image_create(lv_screen_active());
			lv_image_set_src(obj, &img_benchmark_lvgl_logo_argb);
			if (x == 0)
				lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			lv_image_set_rotation(obj, rnd_next(100, 3500));
			shake_anim(obj, 80);
		}
	}
}

static void multiple_labels_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(lv_screen_active(), 80, 0);

	lv_point_t s;
	lv_text_get_size(&s, "Hello LVGL!", lv_obj_get_style_text_font(lv_screen_active(), 0), 0, 0,
			 LV_COORD_MAX, LV_TEXT_FLAG_NONE);

	int32_t cnt = (lv_display_get_horizontal_resolution(NULL) - 16) / (s.x + 30);
	cnt *= ((lv_display_get_vertical_resolution(NULL) - 200) / (s.y + 50));
	if (cnt < 1)
		cnt = 1;

	for (int32_t i = 0; i < cnt; i++) {
		lv_obj_t *obj = lv_label_create(lv_screen_active());
		lv_label_set_text(obj, "Hello LVGL!");
		color_anim(obj);
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

	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *obj = lv_label_create(scr);
	lv_obj_set_width(obj, lv_pct(100));
	lv_label_set_text(obj, txt);
	lv_obj_update_layout(obj);
	scroll_anim(scr, lv_obj_get_scroll_bottom(scr));
}

static void multiple_arcs_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);

	int32_t hor = (lv_display_get_horizontal_resolution(NULL) - 16) / lv_dpx(160);
	int32_t ver = (lv_display_get_vertical_resolution(NULL) - 16) / lv_dpx(160);
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *obj = lv_arc_create(lv_screen_active());
			if (x == 0)
				lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			lv_obj_set_size(obj, lv_dpx(100), lv_dpx(100));
			lv_obj_center(obj);
			lv_arc_set_bg_angles(obj, 0, 360);
			lv_obj_set_style_margin_all(obj, lv_dpx(20), 0);
			lv_obj_set_style_arc_opa(obj, 0, LV_PART_MAIN);
			lv_obj_set_style_bg_opa(obj, 0, LV_PART_KNOB);
			lv_obj_set_style_arc_width(obj, 10, LV_PART_INDICATOR);
			lv_obj_set_style_arc_rounded(obj, false, LV_PART_INDICATOR);
			lv_obj_set_style_arc_color(obj, lv_color_hex3(rnd_next(0x00f, 0xff0)),
						   LV_PART_INDICATOR);
			arc_anim(obj);
		}
	}
}

static void containers_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);

	int32_t hor = ((int32_t)lv_display_get_horizontal_resolution(NULL) - 16) / 300;
	int32_t ver = ((int32_t)lv_display_get_vertical_resolution(NULL) - 16) / 150;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *card = card_create();
			if (x == 0)
				lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			shake_anim(card, 30);
		}
	}
}

static void containers_with_overlay_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);

	int32_t hor = ((int32_t)lv_display_get_horizontal_resolution(NULL) - 16) / 300;
	int32_t ver = ((int32_t)lv_display_get_vertical_resolution(NULL) - 16) / 150;
	if (hor < 1)
		hor = 1;
	if (ver < 1)
		ver = 1;

	for (int32_t y = 0; y < ver; y++) {
		for (int32_t x = 0; x < hor; x++) {
			lv_obj_t *card = card_create();
			if (x == 0)
				lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
			shake_anim(card, 30);
		}
	}

	lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_50, 0);
	color_anim(lv_layer_top());
}

static void containers_with_opa_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);

	int32_t hor = ((int32_t)lv_display_get_horizontal_resolution(NULL) - 16) / 300;
	int32_t ver = ((int32_t)lv_display_get_vertical_resolution(NULL) - 16) / 150;
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
			shake_anim(card, 30);
		}
	}
}

static void containers_with_opa_layer_cb(void)
{
	lv_obj_set_flex_flow(lv_screen_active(), LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lv_screen_active(), LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);

	int32_t hor = ((int32_t)lv_display_get_horizontal_resolution(NULL) - 16) / 300;
	int32_t ver = ((int32_t)lv_display_get_vertical_resolution(NULL) - 16) / 150;
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
			shake_anim(card, 30);
		}
	}
}

static void containers_with_scrolling_cb(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_START);

	for (uint32_t i = 0; i < 50; i++)
		card_create();

	lv_obj_update_layout(scr);
	scroll_anim(scr, lv_obj_get_scroll_bottom(scr));
}

/* ── Widgets demo scene ────────────────────────────────────────────── */

static lv_obj_t *g_tabview;
static uint32_t g_slideshow_tab;

static void slideshow_scroll_cb(void *var, int32_t v)
{
	lv_obj_scroll_to_y(var, v, LV_ANIM_OFF);
}

static void slideshow_ready_cb(lv_anim_t *a)
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
	tab = lv_obj_get_child(tab, (int32_t)g_slideshow_tab);
	if (!tab)
		return;

	lv_obj_update_layout(tab);
	int32_t bot = lv_obj_get_scroll_bottom(tab);
	if (bot <= 0)
		bot = 1;

	uint32_t spd = lv_anim_speed(lv_display_get_dpi(NULL));
	benchmark_anim_slideshow(tab, slideshow_scroll_cb, bot, spd, slideshow_ready_cb);
}

static void gauge_arc_exec_cb(void *var, int32_t v)
{
	lv_arc_set_value(var, v);
}

static void widgets_demo_cb(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_set_style_pad_top(scr, 0, 0);

	/* ── Tabview ─────────────────────────────────── */

	g_tabview = lv_tabview_create(scr);
	lv_tabview_set_tab_bar_size(g_tabview, 40);

	lv_obj_t *tab1 = lv_tabview_add_tab(g_tabview, "Form");
	lv_obj_t *tab2 = lv_tabview_add_tab(g_tabview, "Gauges");
	lv_obj_t *tab3 = lv_tabview_add_tab(g_tabview, "Pickers");

	/* ── Tab 1: Form widgets ─────────────────────── */

	lv_obj_set_flex_flow(tab1, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_gap(tab1, 10, 0);

	lv_obj_t *ta = lv_textarea_create(tab1);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_placeholder_text(ta, "Username");
	lv_obj_set_width(ta, lv_pct(90));

	lv_obj_t *dd = lv_dropdown_create(tab1);
	lv_dropdown_set_options(dd, "Option A\nOption B\nOption C");
	lv_obj_set_width(dd, lv_pct(90));

	lv_obj_t *slider = lv_slider_create(tab1);
	lv_slider_set_value(slider, 40, LV_ANIM_OFF);
	lv_obj_set_width(slider, lv_pct(90));

	lv_obj_t *sw = lv_switch_create(tab1);
	lv_obj_add_state(sw, LV_STATE_CHECKED);

	lv_obj_t *cb = lv_checkbox_create(tab1);
	lv_checkbox_set_text(cb, "I agree");

	lv_obj_t *btn = lv_button_create(tab1);
	lv_obj_set_width(btn, lv_pct(90));
	lv_obj_t *btn_label = lv_label_create(btn);
	lv_label_set_text(btn_label, "Submit");
	lv_obj_center(btn_label);

	/* ── Tab 2: Gauges (Scale + Arc animations) ──── */

	lv_obj_set_flex_flow(tab2, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(tab2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_gap(tab2, 10, 0);

	/* Gauge 1: circular 360° with 3 concentric arcs */
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
			lv_obj_t *arc = lv_arc_create(gauge_box);
			lv_obj_set_size(arc, 180 - arcs[i].margin * 2, 180 - arcs[i].margin * 2);
			lv_obj_center(arc);
			lv_arc_set_range(arc, 0, 100);
			lv_arc_set_bg_angles(arc, 0, 360);
			lv_obj_set_style_arc_opa(arc, 0, LV_PART_MAIN);
			lv_obj_set_style_bg_opa(arc, 0, LV_PART_KNOB);
			lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
			lv_obj_set_style_arc_color(arc, lv_palette_main(arcs[i].pal),
						   LV_PART_INDICATOR);

			lv_anim_t a;
			lv_anim_init(&a);
			lv_anim_set_var(&a, arc);
			lv_anim_set_exec_cb(&a, gauge_arc_exec_cb);
			lv_anim_set_values(&a, 20, 100);
			lv_anim_set_duration(&a, arcs[i].t1);
			lv_anim_set_playback_duration(&a, arcs[i].t2);
			lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
			lv_anim_start(&a);
		}
	}

	/* Gauge 2: semi-circular 270° with sections */
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
		lv_obj_t *arc = lv_arc_create(gauge_box);
		lv_obj_set_size(arc, 160, 160);
		lv_obj_center(arc);
		lv_arc_set_range(arc, 10, 60);
		lv_arc_set_bg_angles(arc, 0, 270);
		lv_arc_set_rotation(arc, 135);
		lv_obj_set_style_arc_opa(arc, 0, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(arc, 0, LV_PART_KNOB);
		lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);

		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, arc);
		lv_anim_set_exec_cb(&a, gauge_arc_exec_cb);
		lv_anim_set_values(&a, 10, 60);
		lv_anim_set_duration(&a, 4100);
		lv_anim_set_playback_duration(&a, 800);
		lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
		lv_anim_start(&a);
	}

	/* Line chart: 12 points */
	{
		lv_obj_t *chart = lv_chart_create(tab2);
		lv_obj_set_size(chart, 200, 140);
		lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
		lv_chart_set_point_count(chart, 12);
		lv_chart_series_t *ser = lv_chart_add_series(
			chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
		static const int32_t data[] = {10, 20, 30, 25, 40, 35, 50, 60, 55, 70, 65, 80};
		for (int i = 0; i < 12; i++)
			lv_chart_set_value_by_id(chart, ser, i, data[i]);
	}

	/* ── Tab 3: Pickers ──────────────────────────── */

	lv_obj_set_flex_flow(tab3, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(tab3, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_gap(tab3, 10, 0);

	lv_obj_t *cal = lv_calendar_create(tab3);
	lv_obj_set_size(cal, 200, 200);
	lv_calendar_set_today_date(cal, 2026, 4, 13);
	lv_calendar_set_showed_date(cal, 2026, 4);

	lv_obj_t *roller = lv_roller_create(tab3);
	lv_roller_set_options(roller, "Mon\nTue\nWed\nThu\nFri\nSat\nSun", LV_ROLLER_MODE_NORMAL);
	lv_roller_set_visible_row_count(roller, 3);

	lv_obj_t *spinbox = lv_spinbox_create(tab3);
	lv_spinbox_set_range(spinbox, 0, 100);
	lv_spinbox_set_value(spinbox, 42);
	lv_spinbox_set_step(spinbox, 1);

	/* Bar chart: 7 points */
	{
		lv_obj_t *chart = lv_chart_create(tab3);
		lv_obj_set_size(chart, 200, 140);
		lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
		lv_chart_set_point_count(chart, 7);
		lv_chart_series_t *ser = lv_chart_add_series(
			chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
		static const int32_t data[] = {40, 55, 30, 70, 50, 65, 45};
		for (int i = 0; i < 7; i++)
			lv_chart_set_value_by_id(chart, ser, i, data[i]);
	}

	/* ── Start slideshow ─────────────────────────── */

	g_slideshow_tab = 0;

	lv_obj_update_layout(tab1);
	int32_t bot = lv_obj_get_scroll_bottom(tab1);
	if (bot <= 0)
		bot = 1;

	uint32_t spd = lv_anim_speed(lv_display_get_dpi(NULL));
	benchmark_anim_slideshow(tab1, slideshow_scroll_cb, bot, spd, slideshow_ready_cb);
}

/* ── Scene array ───────────────────────────────────────────────────── */

static scene_dsc_t scenes[] = {
	{.name = "Empty screen", .scene_time = 3000, .create_cb = empty_screen_cb},
	{.name = "Moving wallpaper", .scene_time = 3000, .create_cb = moving_wallpaper_cb},
	{.name = "Single rectangle", .scene_time = 3000, .create_cb = single_rectangle_cb},
	{.name = "Multiple rectangles", .scene_time = 3000, .create_cb = multiple_rectangles_cb},
	{.name = "Multiple RGB images", .scene_time = 3000, .create_cb = multiple_rgb_images_cb},
	{.name = "Multiple ARGB images", .scene_time = 3000, .create_cb = multiple_argb_images_cb},
	{.name = "Rotated ARGB images", .scene_time = 3000, .create_cb = rotated_argb_images_cb},
	{.name = "Multiple labels", .scene_time = 3000, .create_cb = multiple_labels_cb},
	{.name = "Screen sized text", .scene_time = 5000, .create_cb = screen_sized_text_cb},
	{.name = "Multiple arcs", .scene_time = 3000, .create_cb = multiple_arcs_cb},
	{.name = "Containers", .scene_time = 3000, .create_cb = containers_cb},
	{.name = "Containers with overlay",
	 .scene_time = 3000,
	 .create_cb = containers_with_overlay_cb},
	{.name = "Containers with opa", .scene_time = 3000, .create_cb = containers_with_opa_cb},
	{.name = "Containers with opa_layer",
	 .scene_time = 3000,
	 .create_cb = containers_with_opa_layer_cb},
	{.name = "Containers with scrolling",
	 .scene_time = 5000,
	 .create_cb = containers_with_scrolling_cb},
	{.name = "Widgets demo", .scene_time = 20000, .create_cb = widgets_demo_cb},
	{.name = "", .create_cb = NULL}};

static uint32_t scene_act;
static uint32_t rnd_act;

/* ── Scene management ──────────────────────────────────────────────── */

static void load_scene(uint32_t scene)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_clean(scr);
	lv_obj_set_style_bg_color(scr, lv_palette_lighten(LV_PALETTE_GREY, 4), 0);
	lv_obj_set_style_text_color(scr, lv_color_black(), 0);
	lv_obj_set_style_pad_all(scr, 8, 0);
	lv_obj_set_style_pad_top(scr, 40, 0);
	lv_obj_set_style_pad_gap(scr, 8, 0);
	lv_obj_set_layout(scr, LV_LAYOUT_NONE);
	lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_anim_delete(scr, scroll_anim_y_cb);
	lv_anim_delete(scr, shake_anim_y_cb);
	lv_anim_delete(scr, color_anim_cb);

	lv_anim_delete(lv_layer_top(), color_anim_cb);
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

/* ── Performance observer ──────────────────────────────────────────── */

#if LV_USE_PERF_MONITOR
static void sysmon_perf_observer_cb(lv_observer_t *observer, lv_subject_t *subject)
{
	benchmark_perf_metrics_t m;
	benchmark_extract_perf_metrics(lv_subject_get_pointer(subject), &m);
	lv_obj_t *label = lv_observer_get_target(observer);

	char scene_name[64];
	if (scenes[scene_act].name[0] != '\0')
		snprintf(scene_name, sizeof(scene_name), "%s: ", scenes[scene_act].name);
	else
		scene_name[0] = '\0';

	lv_label_set_text_fmt(label,
			      "%s%u FPS, %u%% CPU\n"
			      "refr. %u ms = %u ms render + %u ms flush",
			      scene_name, (unsigned)m.fps, (unsigned)m.cpu,
			      (unsigned)(m.render_avg_time + m.flush_avg_time),
			      (unsigned)m.render_avg_time, (unsigned)m.flush_avg_time);

	/* Ignore first call — stale data from previous scene */
	if (scenes[scene_act].measurement_cnt != 0) {
		scenes[scene_act].cpu_avg_usage += m.cpu;
		scenes[scene_act].fps_avg += m.fps;
		scenes[scene_act].render_avg_time += m.render_avg_time;
		scenes[scene_act].flush_avg_time += m.flush_avg_time;
	}
	scenes[scene_act].measurement_cnt++;
}
#endif

/* ── Summary table ─────────────────────────────────────────────────── */

static void table_draw_task_event_cb(lv_event_t *e)
{
	lv_draw_task_t *t = lv_event_get_draw_task(e);
	lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
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

static void summary_create(void)
{
	lv_obj_clean(lv_screen_active());
	lv_obj_set_style_pad_hor(lv_screen_active(), 0, 0);

	lv_obj_t *table = lv_table_create(lv_screen_active());
	lv_obj_set_width(table, lv_pct(100));
	lv_obj_set_style_max_height(table, lv_pct(100), 0);
	lv_obj_add_flag(table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
	lv_obj_set_style_text_font(table, &lv_font_montserrat_14, LV_PART_ITEMS);
	lv_obj_set_style_text_font(table, &lv_font_montserrat_14, LV_PART_MAIN);
	lv_obj_set_style_pad_top(table, 2, LV_PART_ITEMS);
	lv_obj_set_style_pad_bottom(table, 2, LV_PART_ITEMS);
	lv_obj_set_style_pad_left(table, 4, LV_PART_ITEMS);
	lv_obj_set_style_pad_right(table, 4, LV_PART_ITEMS);
	lv_obj_set_style_text_color(table, lv_palette_darken(LV_PALETTE_BLUE_GREY, 2),
				    LV_PART_ITEMS);
	lv_obj_set_style_border_color(table, lv_palette_darken(LV_PALETTE_BLUE_GREY, 2),
				      LV_PART_ITEMS);
	lv_obj_add_event_cb(table, table_draw_task_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

	lv_table_set_cell_value(table, 0, 0, "Name");
	lv_table_set_cell_value(table, 0, 1, "Avg. CPU");
	lv_table_set_cell_value(table, 0, 2, "Avg. FPS");
	lv_table_set_cell_value(table, 0, 3, "Avg. time (render + flush)");

	OVE_LOG_INF("Benchmark Summary");
	OVE_LOG_INF("Name, Avg. CPU, Avg. FPS, Avg. time, render, flush");

	lv_obj_update_layout(table);
	int32_t col_w = lv_obj_get_content_width(table) / 4;
	for (int c = 0; c < 4; c++)
		lv_table_set_column_width(table, c, col_w);

	int32_t total_fps = 0, total_cpu = 0;
	int32_t total_render = 0, total_flush = 0;
	int32_t valid = 0;

	for (uint32_t i = 0; scenes[i].create_cb; i++) {
		lv_table_set_cell_value(table, i + 2, 0, scenes[i].name);

		if (scenes[i].measurement_cnt <= 1) {
			lv_table_set_cell_value(table, i + 2, 1, "N/A");
			lv_table_set_cell_value(table, i + 2, 2, "N/A");
			lv_table_set_cell_value(table, i + 2, 3, "N/A");
		} else {
			int32_t cnt = scenes[i].measurement_cnt - 1;
			uint32_t cpu = scenes[i].cpu_avg_usage / cnt;
			uint32_t fps = scenes[i].fps_avg / cnt;
			uint32_t render = scenes[i].render_avg_time / cnt;
			uint32_t flush = scenes[i].flush_avg_time / cnt;

			lv_table_set_cell_value_fmt(table, i + 2, 1, "%u %%", (unsigned)cpu);
			lv_table_set_cell_value_fmt(table, i + 2, 2, "%u FPS", (unsigned)fps);
			lv_table_set_cell_value_fmt(table, i + 2, 3, "%u ms (%u + %u)",
						    (unsigned)(render + flush), (unsigned)render,
						    (unsigned)flush);

			OVE_LOG_INF("%s, %u%%, %u, %u, %u, %u", scenes[i].name, (unsigned)cpu,
				    (unsigned)fps, (unsigned)(render + flush), (unsigned)render,
				    (unsigned)flush);

			valid++;
			total_cpu += cpu;
			total_fps += fps;
			total_render += render;
			total_flush += flush;
		}
	}

	lv_table_set_cell_value(table, 1, 0, "All scenes avg.");
	if (valid < 1) {
		lv_table_set_cell_value(table, 1, 1, "N/A");
		lv_table_set_cell_value(table, 1, 2, "N/A");
		lv_table_set_cell_value(table, 1, 3, "N/A");
	} else {
		uint32_t avg_cpu = total_cpu / valid;
		uint32_t avg_fps = total_fps / valid;
		uint32_t avg_render = total_render / valid;
		uint32_t avg_flush = total_flush / valid;

		lv_table_set_cell_value_fmt(table, 1, 1, "%u %%", (unsigned)avg_cpu);
		lv_table_set_cell_value_fmt(table, 1, 2, "%u FPS", (unsigned)avg_fps);
		lv_table_set_cell_value_fmt(table, 1, 3, "%u ms (%u + %u)",
					    (unsigned)(avg_render + avg_flush),
					    (unsigned)avg_render, (unsigned)avg_flush);

		OVE_LOG_INF("All avg, %u%%, %u, %u, %u, %u", (unsigned)avg_cpu, (unsigned)avg_fps,
			    (unsigned)(avg_render + avg_flush), (unsigned)avg_render,
			    (unsigned)avg_flush);
	}
}

/* ── Animation helpers ─────────────────────────────────────────────── */

static void color_anim_cb(void *var, int32_t v)
{
	(void)v;
	lv_obj_set_style_bg_color(var, lv_color_hex3(rnd_next(0x00f, 0xff0)), 0);
	lv_obj_set_style_text_color(var, lv_color_hex3(rnd_next(0x00f, 0xff0)), 0);
}

static void color_anim(lv_obj_t *obj)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_exec_cb(&a, color_anim_cb);
	lv_anim_set_values(&a, 0, 100);
	lv_anim_set_duration(&a, 100);
	lv_anim_set_var(&a, obj);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

static void arc_anim_cb(void *var, int32_t v)
{
	lv_arc_set_value(var, v);
}

static void arc_anim(lv_obj_t *obj)
{
	uint32_t t1 = rnd_next(1000, 3000);
	uint32_t t2 = rnd_next(1000, 3000);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_exec_cb(&a, arc_anim_cb);
	lv_anim_set_values(&a, 0, 100);
	lv_anim_set_duration(&a, t1);
	lv_anim_set_playback_duration(&a, t2);
	lv_anim_set_var(&a, obj);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

static void scroll_anim_y_cb(void *var, int32_t v)
{
	lv_obj_scroll_to_y(var, v, LV_ANIM_OFF);
}

static void scroll_anim(lv_obj_t *obj, int32_t y_max)
{
	uint32_t t = lv_anim_speed(lv_display_get_dpi(NULL));

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, scroll_anim_y_cb);
	lv_anim_set_values(&a, 0, y_max);
	lv_anim_set_duration(&a, t);
	lv_anim_set_playback_duration(&a, t);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

static void shake_anim_y_cb(void *var, int32_t v)
{
	lv_obj_set_style_translate_y(var, v, 0);
}

static void shake_anim(lv_obj_t *obj, int32_t y_max)
{
	uint32_t t1 = rnd_next(300, 3000);
	uint32_t t2 = rnd_next(300, 3000);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, shake_anim_y_cb);
	lv_anim_set_values(&a, 0, y_max);
	lv_anim_set_duration(&a, t1);
	lv_anim_set_playback_duration(&a, t2);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

/* ── Card composite widget ─────────────────────────────────────────── */

static lv_obj_t *card_create(void)
{
	lv_obj_t *panel = lv_obj_create(lv_screen_active());
	lv_obj_set_size(panel, 270, 120);
	lv_obj_set_style_pad_all(panel, 8, 0);

	lv_obj_t *child = lv_image_create(panel);
	lv_obj_align(child, LV_ALIGN_LEFT_MID, 0, 0);
	lv_image_set_src(child, &img_benchmark_avatar);

	child = lv_label_create(panel);
	lv_label_set_text(child, "John Smith");
#if LV_FONT_MONTSERRAT_24
	lv_obj_set_style_text_font(child, &lv_font_montserrat_24, 0);
#endif
	lv_obj_set_pos(child, 100, 0);

	child = lv_label_create(panel);
	lv_label_set_text(child, "A DIY enthusiast");
#if LV_FONT_MONTSERRAT_14
	lv_obj_set_style_text_font(child, &lv_font_montserrat_14, 0);
#endif
	lv_obj_set_pos(child, 100, 30);

	child = lv_button_create(panel);
	lv_obj_set_pos(child, 100, 50);

	child = lv_label_create(child);
	lv_label_set_text(child, "Connect");

	return panel;
}

/* ── PRNG ──────────────────────────────────────────────────────────── */

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

/* ── Graphics thread ───────────────────────────────────────────────── */

static void graphics_thread(void *arg)
{
	(void)arg;
	uint64_t last_us = 0;

	ove_time_get_us(&last_us);

	while (1) {
		uint64_t now_us = 0;
		ove_time_get_us(&now_us);
		uint32_t elapsed_ms = (uint32_t)((now_us - last_us) / 1000);
		last_us = now_us;

		ove_lvgl_lock();
		ove_lvgl_tick(elapsed_ms);
		ove_lvgl_handler();
		ove_lvgl_unlock();

		ove_thread_sleep_ms(33);
	}
}

/* ── Entry point ───────────────────────────────────────────────────── */

void ove_main(void)
{
	int ret;

	OVE_LOG_INF("LVGL benchmark (heap mode): init");

	ove_thread_t graphics;
	if (ove_thread_create(&graphics, "graphics", graphics_thread, NULL, OVE_PRIO_HIGH, 4096) !=
	    OVE_OK) {
		OVE_LOG_ERR("Failed to spawn graphics thread");
		return;
	}

	/* Init LVGL */
	ret = ove_lvgl_init();
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to init LVGL: %d", ret);
		return;
	}

	/* Setup benchmark */
	ove_lvgl_lock();

	scene_act = 0;

	lv_obj_t *scr = lv_screen_active();
	lv_obj_remove_style_all(scr);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_color(scr, lv_palette_lighten(LV_PALETTE_GREY, 4), 0);
	lv_obj_set_style_pad_all(scr, 8, 0);
	lv_obj_set_style_pad_top(scr, 40, 0);
	lv_obj_set_style_pad_gap(scr, 8, 0);

	lv_obj_t *title = lv_label_create(lv_layer_top());
	lv_obj_set_style_bg_opa(title, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(title, lv_color_white(), 0);
	lv_obj_set_style_text_color(title, lv_color_black(), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
	lv_obj_set_width(title, lv_pct(100));

	load_scene(scene_act);

	lv_timer_create(next_scene_timer_cb, scenes[0].scene_time, NULL);

#if LV_USE_PERF_MONITOR
	lv_subject_t *perf_subj = benchmark_get_perf_subject();
	if (perf_subj)
		lv_subject_add_observer_obj(perf_subj, sysmon_perf_observer_cb, title, NULL);
	else
		lv_label_set_text(title, "Perf monitor unavailable");
#else
	lv_label_set_text(title, "LV_USE_PERF_MONITOR is not enabled");
#endif

	ove_lvgl_unlock();

	OVE_LOG_INF("LVGL benchmark (C): running");
	ove_run();
}
