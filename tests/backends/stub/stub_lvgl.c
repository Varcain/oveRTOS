/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <stddef.h>
#include "ove/ove.h"
#include "lvgl/lvgl.h"

int ove_lvgl_init(void)
{
	return OVE_OK;
}

void ove_lvgl_lock(void)
{
}

void ove_lvgl_unlock(void)
{
}

void ove_lvgl_tick(uint32_t ms)
{
	(void)ms;
}

void ove_lvgl_handler(void)
{
}

/* ── Object core ─────────────────────────────────────────────────── */

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_obj_delete(lv_obj_t *obj)
{
	(void)obj;
}

void lv_obj_clean(lv_obj_t *obj)
{
	(void)obj;
}

lv_obj_t *lv_obj_get_parent(lv_obj_t *obj)
{
	(void)obj;
	return NULL;
}

uint32_t lv_obj_get_child_count(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

int32_t lv_obj_get_width(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

int32_t lv_obj_get_height(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h)
{
	(void)obj;
	(void)w;
	(void)h;
}

void lv_obj_set_width(lv_obj_t *obj, int32_t w)
{
	(void)obj;
	(void)w;
}

void lv_obj_set_height(lv_obj_t *obj, int32_t h)
{
	(void)obj;
	(void)h;
}

void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y)
{
	(void)obj;
	(void)x;
	(void)y;
}

void lv_obj_center(lv_obj_t *obj)
{
	(void)obj;
}

void lv_obj_align(lv_obj_t *obj, int32_t align, int32_t x_ofs, int32_t y_ofs)
{
	(void)obj;
	(void)align;
	(void)x_ofs;
	(void)y_ofs;
}

void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag)
{
	(void)obj;
	(void)flag;
}

void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag)
{
	(void)obj;
	(void)flag;
}

void lv_obj_add_state(lv_obj_t *obj, uint32_t state)
{
	(void)obj;
	(void)state;
}

void lv_obj_remove_state(lv_obj_t *obj, uint32_t state)
{
	(void)obj;
	(void)state;
}

void lv_obj_set_user_data(lv_obj_t *obj, void *data)
{
	(void)obj;
	(void)data;
}

void *lv_obj_get_user_data(lv_obj_t *obj)
{
	(void)obj;
	return NULL;
}

void lv_obj_set_flex_flow(lv_obj_t *obj, uint32_t flow)
{
	(void)obj;
	(void)flow;
}

/* ── Events ──────────────────────────────────────────────────────── */

void lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb,
                         lv_event_code_t code, void *user_data)
{
	(void)obj;
	(void)cb;
	(void)code;
	(void)user_data;
}

void *lv_event_get_user_data(lv_event_t *e)
{
	(void)e;
	return NULL;
}

lv_obj_t *lv_event_get_target(lv_event_t *e)
{
	(void)e;
	return NULL;
}

/* ── Inline styles ───────────────────────────────────────────────── */

void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, uint32_t sel)
{
	(void)obj;
	(void)color;
	(void)sel;
}

void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, uint32_t sel)
{
	(void)obj;
	(void)opa;
	(void)sel;
}

void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color, uint32_t sel)
{
	(void)obj;
	(void)color;
	(void)sel;
}

void lv_obj_set_style_border_width(lv_obj_t *obj, int32_t w, uint32_t sel)
{
	(void)obj;
	(void)w;
	(void)sel;
}

void lv_obj_set_style_radius(lv_obj_t *obj, int32_t radius, uint32_t sel)
{
	(void)obj;
	(void)radius;
	(void)sel;
}

void lv_obj_set_style_pad_top(lv_obj_t *obj, int32_t p, uint32_t sel)
{
	(void)obj;
	(void)p;
	(void)sel;
}

void lv_obj_set_style_pad_bottom(lv_obj_t *obj, int32_t p, uint32_t sel)
{
	(void)obj;
	(void)p;
	(void)sel;
}

void lv_obj_set_style_pad_left(lv_obj_t *obj, int32_t p, uint32_t sel)
{
	(void)obj;
	(void)p;
	(void)sel;
}

void lv_obj_set_style_pad_right(lv_obj_t *obj, int32_t p, uint32_t sel)
{
	(void)obj;
	(void)p;
	(void)sel;
}

void lv_obj_set_style_pad_row(lv_obj_t *obj, int32_t g, uint32_t sel)
{
	(void)obj;
	(void)g;
	(void)sel;
}

void lv_obj_set_style_pad_column(lv_obj_t *obj, int32_t g, uint32_t sel)
{
	(void)obj;
	(void)g;
	(void)sel;
}

void lv_obj_set_style_pad_all(lv_obj_t *obj, int32_t p, uint32_t sel)
{
	(void)obj;
	(void)p;
	(void)sel;
}

void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, uint32_t sel)
{
	(void)obj;
	(void)color;
	(void)sel;
}

void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, uint32_t sel)
{
	(void)obj;
	(void)font;
	(void)sel;
}

/* ── Style object ────────────────────────────────────────────────── */

void lv_style_init(lv_style_t *style)
{
	(void)style;
}

void lv_style_reset(lv_style_t *style)
{
	(void)style;
}

void lv_style_set_bg_color(lv_style_t *style, lv_color_t color)
{
	(void)style;
	(void)color;
}

void lv_style_set_bg_opa(lv_style_t *style, lv_opa_t opa)
{
	(void)style;
	(void)opa;
}

void lv_style_set_radius(lv_style_t *style, int32_t r)
{
	(void)style;
	(void)r;
}

void lv_style_set_border_color(lv_style_t *style, lv_color_t color)
{
	(void)style;
	(void)color;
}

void lv_style_set_border_width(lv_style_t *style, int32_t w)
{
	(void)style;
	(void)w;
}

void lv_style_set_pad_top(lv_style_t *style, int32_t p)
{
	(void)style;
	(void)p;
}

void lv_style_set_pad_bottom(lv_style_t *style, int32_t p)
{
	(void)style;
	(void)p;
}

void lv_style_set_pad_left(lv_style_t *style, int32_t p)
{
	(void)style;
	(void)p;
}

void lv_style_set_pad_right(lv_style_t *style, int32_t p)
{
	(void)style;
	(void)p;
}

void lv_style_set_pad_all(lv_style_t *style, int32_t p)
{
	(void)style;
	(void)p;
}

void lv_style_set_text_color(lv_style_t *style, lv_color_t color)
{
	(void)style;
	(void)color;
}

void lv_style_set_text_font(lv_style_t *style, const lv_font_t *f)
{
	(void)style;
	(void)f;
}

void lv_obj_add_style(lv_obj_t *obj, lv_style_t *style, uint32_t sel)
{
	(void)obj;
	(void)style;
	(void)sel;
}

void lv_obj_remove_style_all(lv_obj_t *obj)
{
	(void)obj;
}

/* ── Color helpers ───────────────────────────────────────────────── */

lv_color_t lv_palette_main(uint32_t p)
{
	(void)p;
	lv_color_t c = {0, 0, 0};
	return c;
}

lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b)
{
	lv_color_t c = {b, g, r};
	return c;
}

lv_color_t lv_color_white(void)
{
	lv_color_t c = {255, 255, 255};
	return c;
}

lv_color_t lv_color_black(void)
{
	lv_color_t c = {0, 0, 0};
	return c;
}

lv_color_t lv_color_hex(uint32_t hex)
{
	lv_color_t c = {
		(uint8_t)(hex & 0xFF),
		(uint8_t)((hex >> 8) & 0xFF),
		(uint8_t)((hex >> 16) & 0xFF)
	};
	return c;
}

/* ── Label ───────────────────────────────────────────────────────── */

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_label_set_text(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
}

void lv_label_set_text_static(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
}

void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t mode)
{
	(void)obj;
	(void)mode;
}

/* ── Bar ─────────────────────────────────────────────────────────── */

lv_obj_t *lv_bar_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_bar_set_value(lv_obj_t *obj, int32_t value, int32_t anim)
{
	(void)obj;
	(void)value;
	(void)anim;
}

void lv_bar_set_range(lv_obj_t *obj, int32_t min, int32_t max)
{
	(void)obj;
	(void)min;
	(void)max;
}

/* ── Screen ──────────────────────────────────────────────────────── */

lv_obj_t *lv_screen_active(void)
{
	return NULL;
}

/* ── Fonts ───────────────────────────────────────────────────────── */

const lv_font_t lv_font_montserrat_32 = {NULL};
const lv_font_t lv_font_montserrat_14 = {NULL};
