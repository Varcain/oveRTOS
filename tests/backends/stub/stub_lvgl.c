/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <stddef.h>
#include <stdbool.h>
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

void lv_obj_set_x(lv_obj_t *obj, int32_t x)
{
	(void)obj;
	(void)x;
}

void lv_obj_set_y(lv_obj_t *obj, int32_t y)
{
	(void)obj;
	(void)y;
}

int32_t lv_obj_get_x(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

int32_t lv_obj_get_y(lv_obj_t *obj)
{
	(void)obj;
	return 0;
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

bool lv_obj_has_state(lv_obj_t *obj, uint32_t state)
{
	(void)obj;
	(void)state;
	return false;
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

void lv_obj_set_flex_align(lv_obj_t *obj, uint32_t main, uint32_t cross, uint32_t track)
{
	(void)obj;
	(void)main;
	(void)cross;
	(void)track;
}

void lv_obj_set_flex_grow(lv_obj_t *obj, uint8_t grow)
{
	(void)obj;
	(void)grow;
}

/* ── Grid ───────────────────────────────────────────────────────── */

void lv_obj_set_grid_dsc_array(lv_obj_t *obj, const int32_t *col_dsc,
                               const int32_t *row_dsc)
{
	(void)obj;
	(void)col_dsc;
	(void)row_dsc;
}

void lv_obj_set_grid_cell(lv_obj_t *obj, uint32_t col_align, int32_t col_pos,
                          int32_t col_span, uint32_t row_align, int32_t row_pos,
                          int32_t row_span)
{
	(void)obj;
	(void)col_align;
	(void)col_pos;
	(void)col_span;
	(void)row_align;
	(void)row_pos;
	(void)row_span;
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

void lv_obj_set_style_text_align(lv_obj_t *obj, uint32_t align, uint32_t sel)
{
	(void)obj;
	(void)align;
	(void)sel;
}

void lv_obj_set_style_opa(lv_obj_t *obj, uint8_t opa, uint32_t sel)
{
	(void)obj;
	(void)opa;
	(void)sel;
}

void lv_obj_set_style_arc_color(lv_obj_t *obj, lv_color_t color, uint32_t sel)
{
	(void)obj;
	(void)color;
	(void)sel;
}

void lv_obj_set_style_arc_width(lv_obj_t *obj, int32_t w, uint32_t sel)
{
	(void)obj;
	(void)w;
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

void lv_label_bind_text(lv_obj_t *obj, lv_subject_t *subject, const char *fmt)
{
	(void)obj;
	(void)subject;
	(void)fmt;
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

/* ── Button ──────────────────────────────────────────────────────── */

lv_obj_t *lv_button_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

/* ── Slider ──────────────────────────────────────────────────────── */

lv_obj_t *lv_slider_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_slider_set_value(lv_obj_t *obj, int32_t value, int32_t anim)
{
	(void)obj;
	(void)value;
	(void)anim;
}

void lv_slider_set_range(lv_obj_t *obj, int32_t min, int32_t max)
{
	(void)obj;
	(void)min;
	(void)max;
}

int32_t lv_slider_get_value(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

void lv_slider_bind_value(lv_obj_t *obj, lv_subject_t *subject)
{
	(void)obj;
	(void)subject;
}

/* ── Switch ──────────────────────────────────────────────────────── */

lv_obj_t *lv_switch_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

/* ── Checkbox ────────────────────────────────────────────────────── */

lv_obj_t *lv_checkbox_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_checkbox_set_text(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
}

void lv_checkbox_set_text_static(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
}

/* ── Arc ─────────────────────────────────────────────────────────── */

lv_obj_t *lv_arc_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_arc_set_value(lv_obj_t *obj, int32_t value)
{
	(void)obj;
	(void)value;
}

void lv_arc_set_range(lv_obj_t *obj, int32_t min, int32_t max)
{
	(void)obj;
	(void)min;
	(void)max;
}

void lv_arc_set_bg_angles(lv_obj_t *obj, uint32_t start, uint32_t end)
{
	(void)obj;
	(void)start;
	(void)end;
}

void lv_arc_set_angles(lv_obj_t *obj, uint32_t start, uint32_t end)
{
	(void)obj;
	(void)start;
	(void)end;
}

void lv_arc_set_rotation(lv_obj_t *obj, uint32_t rotation)
{
	(void)obj;
	(void)rotation;
}

int32_t lv_arc_get_value(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

void lv_arc_bind_value(lv_obj_t *obj, lv_subject_t *subject)
{
	(void)obj;
	(void)subject;
}

/* ── Image ───────────────────────────────────────────────────────── */

lv_obj_t *lv_image_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_image_set_src(lv_obj_t *obj, const void *src)
{
	(void)obj;
	(void)src;
}

void lv_image_set_rotation(lv_obj_t *obj, int32_t angle)
{
	(void)obj;
	(void)angle;
}

void lv_image_set_scale(lv_obj_t *obj, uint32_t zoom)
{
	(void)obj;
	(void)zoom;
}

void lv_image_set_pivot(lv_obj_t *obj, int32_t x, int32_t y)
{
	(void)obj;
	(void)x;
	(void)y;
}

/* ── Msgbox ──────────────────────────────────────────────────────── */

lv_obj_t *lv_msgbox_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_msgbox_add_title(lv_obj_t *obj, const char *title)
{
	(void)obj;
	(void)title;
}

void lv_msgbox_add_text(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
}

void lv_msgbox_add_close_button(lv_obj_t *obj)
{
	(void)obj;
}

lv_obj_t *lv_msgbox_add_footer_button(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
	return NULL;
}

lv_obj_t *lv_msgbox_get_content(lv_obj_t *obj)
{
	(void)obj;
	return NULL;
}

lv_obj_t *lv_msgbox_get_header(lv_obj_t *obj)
{
	(void)obj;
	return NULL;
}

lv_obj_t *lv_msgbox_get_footer(lv_obj_t *obj)
{
	(void)obj;
	return NULL;
}

void lv_msgbox_close(lv_obj_t *obj)
{
	(void)obj;
}

/* ── Spinner ─────────────────────────────────────────────────────── */

lv_obj_t *lv_spinner_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_spinner_set_anim_params(lv_obj_t *obj, uint32_t time_ms, uint32_t angle_deg)
{
	(void)obj;
	(void)time_ms;
	(void)angle_deg;
}

/* ── Led ─────────────────────────────────────────────────────────── */

lv_obj_t *lv_led_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_led_set_color(lv_obj_t *obj, lv_color_t color)
{
	(void)obj;
	(void)color;
}

void lv_led_set_brightness(lv_obj_t *obj, uint8_t bright)
{
	(void)obj;
	(void)bright;
}

void lv_led_on(lv_obj_t *obj)
{
	(void)obj;
}

void lv_led_off(lv_obj_t *obj)
{
	(void)obj;
}

void lv_led_toggle(lv_obj_t *obj)
{
	(void)obj;
}

uint8_t lv_led_get_brightness(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

/* ── Textarea ────────────────────────────────────────────────────── */

lv_obj_t *lv_textarea_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_textarea_set_text(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
}

void lv_textarea_add_text(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
}

void lv_textarea_set_placeholder_text(lv_obj_t *obj, const char *text)
{
	(void)obj;
	(void)text;
}

void lv_textarea_set_one_line(lv_obj_t *obj, bool en)
{
	(void)obj;
	(void)en;
}

void lv_textarea_set_password_mode(lv_obj_t *obj, bool en)
{
	(void)obj;
	(void)en;
}

void lv_textarea_set_max_length(lv_obj_t *obj, uint32_t max_len)
{
	(void)obj;
	(void)max_len;
}

void lv_textarea_set_accepted_chars(lv_obj_t *obj, const char *list)
{
	(void)obj;
	(void)list;
}

void lv_textarea_set_cursor_pos(lv_obj_t *obj, int32_t pos)
{
	(void)obj;
	(void)pos;
}

void lv_textarea_set_cursor_click_pos(lv_obj_t *obj, bool en)
{
	(void)obj;
	(void)en;
}

const char *lv_textarea_get_text(lv_obj_t *obj)
{
	(void)obj;
	return "";
}

uint32_t lv_textarea_get_cursor_pos(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

bool lv_textarea_get_password_mode(lv_obj_t *obj)
{
	(void)obj;
	return false;
}

bool lv_textarea_get_one_line(lv_obj_t *obj)
{
	(void)obj;
	return false;
}

void lv_textarea_add_char(lv_obj_t *obj, uint32_t c)
{
	(void)obj;
	(void)c;
}

void lv_textarea_delete_char(lv_obj_t *obj)
{
	(void)obj;
}

/* ── Dropdown ────────────────────────────────────────────────────── */

lv_obj_t *lv_dropdown_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_dropdown_set_options(lv_obj_t *obj, const char *opts)
{
	(void)obj;
	(void)opts;
}

void lv_dropdown_set_options_static(lv_obj_t *obj, const char *opts)
{
	(void)obj;
	(void)opts;
}

void lv_dropdown_add_option(lv_obj_t *obj, const char *opt, uint32_t pos)
{
	(void)obj;
	(void)opt;
	(void)pos;
}

void lv_dropdown_clear_options(lv_obj_t *obj)
{
	(void)obj;
}

void lv_dropdown_set_selected(lv_obj_t *obj, uint32_t sel)
{
	(void)obj;
	(void)sel;
}

uint32_t lv_dropdown_get_selected(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

uint32_t lv_dropdown_get_option_count(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

void lv_dropdown_get_selected_str(lv_obj_t *obj, char *buf, uint32_t buf_size)
{
	(void)obj;
	(void)buf;
	(void)buf_size;
}

void lv_dropdown_set_dir(lv_obj_t *obj, lv_dir_t dir)
{
	(void)obj;
	(void)dir;
}

void lv_dropdown_set_symbol(lv_obj_t *obj, const void *symbol)
{
	(void)obj;
	(void)symbol;
}

bool lv_dropdown_is_open(lv_obj_t *obj)
{
	(void)obj;
	return false;
}

void lv_dropdown_open(lv_obj_t *obj)
{
	(void)obj;
}

void lv_dropdown_close(lv_obj_t *obj)
{
	(void)obj;
}

void lv_dropdown_bind_value(lv_obj_t *obj, lv_subject_t *subject)
{
	(void)obj;
	(void)subject;
}

/* ── Roller ──────────────────────────────────────────────────────── */

lv_obj_t *lv_roller_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_roller_set_options(lv_obj_t *obj, const char *opts, uint32_t mode)
{
	(void)obj;
	(void)opts;
	(void)mode;
}

void lv_roller_set_selected(lv_obj_t *obj, uint32_t sel, int32_t anim)
{
	(void)obj;
	(void)sel;
	(void)anim;
}

void lv_roller_set_visible_row_count(lv_obj_t *obj, uint32_t rows)
{
	(void)obj;
	(void)rows;
}

uint32_t lv_roller_get_selected(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

uint32_t lv_roller_get_option_count(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

void lv_roller_get_selected_str(lv_obj_t *obj, char *buf, uint32_t buf_size)
{
	(void)obj;
	(void)buf;
	(void)buf_size;
}

void lv_roller_bind_value(lv_obj_t *obj, lv_subject_t *subject)
{
	(void)obj;
	(void)subject;
}

/* ── Spinbox ─────────────────────────────────────────────────────── */

lv_obj_t *lv_spinbox_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_spinbox_set_value(lv_obj_t *obj, int32_t val)
{
	(void)obj;
	(void)val;
}

void lv_spinbox_set_range(lv_obj_t *obj, int32_t min, int32_t max)
{
	(void)obj;
	(void)min;
	(void)max;
}

void lv_spinbox_set_step(lv_obj_t *obj, uint32_t step)
{
	(void)obj;
	(void)step;
}

void lv_spinbox_set_digit_format(lv_obj_t *obj, uint32_t digit_count, uint32_t sep_pos)
{
	(void)obj;
	(void)digit_count;
	(void)sep_pos;
}

void lv_spinbox_set_rollover(lv_obj_t *obj, bool en)
{
	(void)obj;
	(void)en;
}

void lv_spinbox_set_cursor_pos(lv_obj_t *obj, uint32_t pos)
{
	(void)obj;
	(void)pos;
}

int32_t lv_spinbox_get_value(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

int32_t lv_spinbox_get_step(lv_obj_t *obj)
{
	(void)obj;
	return 1;
}

void lv_spinbox_increment(lv_obj_t *obj)
{
	(void)obj;
}

void lv_spinbox_decrement(lv_obj_t *obj)
{
	(void)obj;
}

/* ── Keyboard ────────────────────────────────────────────────────── */

lv_obj_t *lv_keyboard_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_keyboard_set_textarea(lv_obj_t *kb, lv_obj_t *ta)
{
	(void)kb;
	(void)ta;
}

void lv_keyboard_set_mode(lv_obj_t *kb, uint32_t mode)
{
	(void)kb;
	(void)mode;
}

void lv_keyboard_set_popovers(lv_obj_t *kb, bool en)
{
	(void)kb;
	(void)en;
}

lv_obj_t *lv_keyboard_get_textarea(lv_obj_t *kb)
{
	(void)kb;
	return NULL;
}

/* ── Chart ───────────────────────────────────────────────────────── */

lv_obj_t *lv_chart_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_chart_set_type(lv_obj_t *obj, uint32_t type)
{
	(void)obj;
	(void)type;
}

void lv_chart_set_point_count(lv_obj_t *obj, uint32_t count)
{
	(void)obj;
	(void)count;
}

void lv_chart_set_axis_range(lv_obj_t *obj, uint32_t axis, int32_t min, int32_t max)
{
	(void)obj;
	(void)axis;
	(void)min;
	(void)max;
}

void lv_chart_set_update_mode(lv_obj_t *obj, uint32_t mode)
{
	(void)obj;
	(void)mode;
}

void lv_chart_set_div_line_count(lv_obj_t *obj, uint32_t hdiv, uint32_t vdiv)
{
	(void)obj;
	(void)hdiv;
	(void)vdiv;
}

lv_chart_series_t *lv_chart_add_series(lv_obj_t *obj, lv_color_t color, uint32_t axis)
{
	(void)obj;
	(void)color;
	(void)axis;
	return NULL;
}

void lv_chart_remove_series(lv_obj_t *obj, lv_chart_series_t *series)
{
	(void)obj;
	(void)series;
}

void lv_chart_set_next_value(lv_obj_t *obj, lv_chart_series_t *series, int32_t value)
{
	(void)obj;
	(void)series;
	(void)value;
}

void lv_chart_set_series_value_by_id(lv_obj_t *obj, lv_chart_series_t *series,
                                     uint32_t id, int32_t value)
{
	(void)obj;
	(void)series;
	(void)id;
	(void)value;
}

/* ── Table ───────────────────────────────────────────────────────── */

lv_obj_t *lv_table_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_table_set_cell_value(lv_obj_t *obj, uint32_t row, uint32_t col, const char *txt)
{
	(void)obj;
	(void)row;
	(void)col;
	(void)txt;
}

void lv_table_set_row_count(lv_obj_t *obj, uint32_t cnt)
{
	(void)obj;
	(void)cnt;
}

void lv_table_set_column_count(lv_obj_t *obj, uint32_t cnt)
{
	(void)obj;
	(void)cnt;
}

void lv_table_set_column_width(lv_obj_t *obj, uint32_t col, int32_t w)
{
	(void)obj;
	(void)col;
	(void)w;
}

const char *lv_table_get_cell_value(lv_obj_t *obj, uint32_t row, uint32_t col)
{
	(void)obj;
	(void)row;
	(void)col;
	return "";
}

uint32_t lv_table_get_row_count(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

uint32_t lv_table_get_column_count(lv_obj_t *obj)
{
	(void)obj;
	return 0;
}

int32_t lv_table_get_column_width(lv_obj_t *obj, uint32_t col)
{
	(void)obj;
	(void)col;
	return 0;
}

/* ── Tabview ─────────────────────────────────────────────────────── */

lv_obj_t *lv_tabview_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

lv_obj_t *lv_tabview_add_tab(lv_obj_t *tv, const char *name)
{
	(void)tv;
	(void)name;
	return NULL;
}

void lv_tabview_rename_tab(lv_obj_t *tv, uint32_t idx, const char *name)
{
	(void)tv;
	(void)idx;
	(void)name;
}

void lv_tabview_set_active(lv_obj_t *tv, uint32_t idx, int32_t anim)
{
	(void)tv;
	(void)idx;
	(void)anim;
}

void lv_tabview_set_tab_bar_position(lv_obj_t *tv, lv_dir_t dir)
{
	(void)tv;
	(void)dir;
}

void lv_tabview_set_tab_bar_size(lv_obj_t *tv, int32_t size)
{
	(void)tv;
	(void)size;
}

uint32_t lv_tabview_get_tab_count(lv_obj_t *tv)
{
	(void)tv;
	return 0;
}

uint32_t lv_tabview_get_tab_active(lv_obj_t *tv)
{
	(void)tv;
	return 0;
}

lv_obj_t *lv_tabview_get_content(lv_obj_t *tv)
{
	(void)tv;
	return NULL;
}

/* ── List ────────────────────────────────────────────────────────── */

lv_obj_t *lv_list_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

lv_obj_t *lv_list_add_text(lv_obj_t *list, const char *text)
{
	(void)list;
	(void)text;
	return NULL;
}

lv_obj_t *lv_list_add_button(lv_obj_t *list, const void *icon, const char *text)
{
	(void)list;
	(void)icon;
	(void)text;
	return NULL;
}

const char *lv_list_get_button_text(lv_obj_t *list, lv_obj_t *btn)
{
	(void)list;
	(void)btn;
	return "";
}

/* ── Canvas ──────────────────────────────────────────────────────── */

lv_obj_t *lv_canvas_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int32_t w, int32_t h,
                          lv_color_format_t cf)
{
	(void)obj;
	(void)buf;
	(void)w;
	(void)h;
	(void)cf;
}

void lv_canvas_fill_bg(lv_obj_t *obj, lv_color_t color, uint8_t opa)
{
	(void)obj;
	(void)color;
	(void)opa;
}

void lv_canvas_set_px(lv_obj_t *obj, int32_t x, int32_t y, lv_color_t color, uint8_t opa)
{
	(void)obj;
	(void)x;
	(void)y;
	(void)color;
	(void)opa;
}

void lv_canvas_init_layer(lv_obj_t *obj, lv_layer_t *layer)
{
	(void)obj;
	(void)layer;
}

void lv_canvas_finish_layer(lv_obj_t *obj, lv_layer_t *layer)
{
	(void)obj;
	(void)layer;
}

/* ── Calendar ────────────────────────────────────────────────────── */

lv_obj_t *lv_calendar_create(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

void lv_calendar_set_today_date(lv_obj_t *obj, uint32_t year, uint32_t month,
                                uint32_t day)
{
	(void)obj;
	(void)year;
	(void)month;
	(void)day;
}

void lv_calendar_set_month_shown(lv_obj_t *obj, uint32_t year, uint32_t month)
{
	(void)obj;
	(void)year;
	(void)month;
}

void lv_calendar_set_highlighted_dates(lv_obj_t *obj, lv_calendar_date_t dates[],
                                       uint32_t cnt)
{
	(void)obj;
	(void)dates;
	(void)cnt;
}

uint32_t lv_calendar_get_pressed_date(lv_obj_t *obj, lv_calendar_date_t *date)
{
	(void)obj;
	(void)date;
	return 0;
}

lv_obj_t *lv_calendar_add_header_arrow(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

lv_obj_t *lv_calendar_add_header_dropdown(lv_obj_t *parent)
{
	(void)parent;
	return NULL;
}

/* ── Screen ──────────────────────────────────────────────────────── */

lv_obj_t *lv_screen_active(void)
{
	return NULL;
}

void lv_screen_load(lv_obj_t *scr)
{
	(void)scr;
}

void lv_screen_load_anim(lv_obj_t *scr, lv_screen_load_anim_t anim,
                         uint32_t time_ms, uint32_t delay_ms, bool auto_del)
{
	(void)scr;
	(void)anim;
	(void)time_ms;
	(void)delay_ms;
	(void)auto_del;
}

/* ── Group (focus navigation) ────────────────────────────────────── */

lv_group_t *lv_group_create(void)
{
	return NULL;
}

void lv_group_delete(lv_group_t *group)
{
	(void)group;
}

void lv_group_set_default(lv_group_t *group)
{
	(void)group;
}

lv_group_t *lv_group_get_default(void)
{
	return NULL;
}

void lv_group_add_obj(lv_group_t *group, lv_obj_t *obj)
{
	(void)group;
	(void)obj;
}

void lv_group_remove_obj(lv_obj_t *obj)
{
	(void)obj;
}

void lv_group_remove_all_objs(lv_group_t *group)
{
	(void)group;
}

void lv_group_focus_obj(lv_obj_t *obj)
{
	(void)obj;
}

void lv_group_focus_next(lv_group_t *group)
{
	(void)group;
}

void lv_group_focus_prev(lv_group_t *group)
{
	(void)group;
}

void lv_group_focus_freeze(lv_group_t *group, bool en)
{
	(void)group;
	(void)en;
}

lv_obj_t *lv_group_get_focused(lv_group_t *group)
{
	(void)group;
	return NULL;
}

void lv_group_set_editing(lv_group_t *group, bool en)
{
	(void)group;
	(void)en;
}

bool lv_group_get_editing(lv_group_t *group)
{
	(void)group;
	return false;
}

uint32_t lv_group_get_obj_count(lv_group_t *group)
{
	(void)group;
	return 0;
}

/* ── Subject / Observer (reactive state) ─────────────────────────── */

void lv_subject_init_int(lv_subject_t *subject, int32_t value)
{
	(void)subject;
	(void)value;
}

void lv_subject_set_int(lv_subject_t *subject, int32_t value)
{
	(void)subject;
	(void)value;
}

int32_t lv_subject_get_int(lv_subject_t *subject)
{
	(void)subject;
	return 0;
}

void lv_subject_deinit(lv_subject_t *subject)
{
	(void)subject;
}

lv_observer_t *lv_subject_add_observer_obj(lv_subject_t *subject,
                                           lv_observer_cb_t cb,
                                           lv_obj_t *obj, void *user_data)
{
	(void)subject;
	(void)cb;
	(void)obj;
	(void)user_data;
	return NULL;
}

void lv_subject_notify(lv_subject_t *subject)
{
	(void)subject;
}

void lv_observer_remove(lv_observer_t *observer)
{
	(void)observer;
}

/* ── Animation ───────────────────────────────────────────────────── */

void lv_anim_init(lv_anim_t *a)
{
	(void)a;
}

void lv_anim_set_var(lv_anim_t *a, void *var)
{
	(void)a;
	(void)var;
}

void lv_anim_set_values(lv_anim_t *a, int32_t start, int32_t end)
{
	(void)a;
	(void)start;
	(void)end;
}

void lv_anim_set_duration(lv_anim_t *a, uint32_t duration)
{
	(void)a;
	(void)duration;
}

void lv_anim_set_delay(lv_anim_t *a, uint32_t delay)
{
	(void)a;
	(void)delay;
}

void lv_anim_set_exec_cb(lv_anim_t *a, lv_anim_exec_xcb_t exec_cb)
{
	(void)a;
	(void)exec_cb;
}

void lv_anim_set_path_cb(lv_anim_t *a, lv_anim_path_cb_t path_cb)
{
	(void)a;
	(void)path_cb;
}

void lv_anim_set_repeat_count(lv_anim_t *a, uint32_t cnt)
{
	(void)a;
	(void)cnt;
}

void lv_anim_set_repeat_delay(lv_anim_t *a, uint32_t delay)
{
	(void)a;
	(void)delay;
}

void lv_anim_set_reverse_duration(lv_anim_t *a, uint32_t duration)
{
	(void)a;
	(void)duration;
}

void lv_anim_set_reverse_delay(lv_anim_t *a, uint32_t delay)
{
	(void)a;
	(void)delay;
}

void lv_anim_set_completed_cb(lv_anim_t *a, lv_anim_completed_cb_t cb)
{
	(void)a;
	(void)cb;
}

void lv_anim_start(const lv_anim_t *a)
{
	(void)a;
}

bool lv_anim_delete(void *var, lv_anim_exec_xcb_t exec_cb)
{
	(void)var;
	(void)exec_cb;
	return false;
}

/* Animation path functions */

int32_t lv_anim_path_linear(const lv_anim_t *a)
{
	(void)a;
	return 0;
}

int32_t lv_anim_path_ease_in(const lv_anim_t *a)
{
	(void)a;
	return 0;
}

int32_t lv_anim_path_ease_out(const lv_anim_t *a)
{
	(void)a;
	return 0;
}

int32_t lv_anim_path_ease_in_out(const lv_anim_t *a)
{
	(void)a;
	return 0;
}

int32_t lv_anim_path_overshoot(const lv_anim_t *a)
{
	(void)a;
	return 0;
}

int32_t lv_anim_path_bounce(const lv_anim_t *a)
{
	(void)a;
	return 0;
}

int32_t lv_anim_path_step(const lv_anim_t *a)
{
	(void)a;
	return 0;
}

/* ── Timer ───────────────────────────────────────────────────────── */

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data)
{
	(void)cb;
	(void)period;
	(void)user_data;
	return NULL;
}

void lv_timer_delete(lv_timer_t *timer)
{
	(void)timer;
}

void lv_timer_pause(lv_timer_t *timer)
{
	(void)timer;
}

void lv_timer_resume(lv_timer_t *timer)
{
	(void)timer;
}

void lv_timer_set_period(lv_timer_t *timer, uint32_t period)
{
	(void)timer;
	(void)period;
}

void lv_timer_set_repeat_count(lv_timer_t *timer, int32_t cnt)
{
	(void)timer;
	(void)cnt;
}

void lv_timer_reset(lv_timer_t *timer)
{
	(void)timer;
}

void lv_timer_ready(lv_timer_t *timer)
{
	(void)timer;
}

void *lv_timer_get_user_data(lv_timer_t *timer)
{
	(void)timer;
	return NULL;
}

/* ── Fonts ───────────────────────────────────────────────────────── */

const lv_font_t lv_font_montserrat_14 = {NULL};
const lv_font_t lv_font_montserrat_16 = {NULL};
const lv_font_t lv_font_montserrat_20 = {NULL};
const lv_font_t lv_font_montserrat_24 = {NULL};
const lv_font_t lv_font_montserrat_32 = {NULL};
const lv_font_t lv_font_montserrat_48 = {NULL};
