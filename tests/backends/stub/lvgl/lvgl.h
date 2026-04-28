/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* Minimal LVGL stub header for test builds where real LVGL is not available.
 * Provides just enough type/function declarations for bindgen to succeed. */

#ifndef LVGL_H
#define LVGL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lv_obj_t lv_obj_t;

typedef struct {
	uint8_t blue;
	uint8_t green;
	uint8_t red;
} lv_color_t;

typedef struct {
	const void *dsc;
} lv_font_t;

typedef struct {
	uint32_t dummy;
} lv_style_t;

typedef uint8_t lv_opa_t;
typedef uint32_t lv_align_t;
typedef uint32_t lv_obj_flag_t;
typedef uint32_t lv_state_t;
typedef bool lv_anim_enable_t;
typedef uint32_t lv_label_long_mode_t;
typedef void lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *e);
typedef uint32_t lv_event_code_t;
typedef uint32_t lv_dir_t;
typedef uint32_t lv_color_format_t;
typedef uint32_t lv_screen_load_anim_t;
typedef uint32_t lv_flex_flow_t;
typedef uint32_t lv_grid_align_t;
typedef uint32_t lv_roller_mode_t;
typedef uint32_t lv_keyboard_mode_t;
typedef uint32_t lv_chart_type_t;
typedef uint32_t lv_chart_axis_t;
typedef uint32_t lv_chart_update_mode_t;

/* Align enum */
enum {
	LV_ALIGN_DEFAULT = 0,
	LV_ALIGN_TOP_LEFT,
	LV_ALIGN_TOP_MID,
	LV_ALIGN_TOP_RIGHT,
	LV_ALIGN_BOTTOM_LEFT,
	LV_ALIGN_BOTTOM_MID,
	LV_ALIGN_BOTTOM_RIGHT,
	LV_ALIGN_LEFT_MID,
	LV_ALIGN_RIGHT_MID,
	LV_ALIGN_CENTER,
};

/* Event codes */
enum {
	LV_EVENT_CLICKED = 7,
	LV_EVENT_VALUE_CHANGED = 28,
	LV_EVENT_DELETE = 36,
};

/* Object flags */
enum {
	LV_OBJ_FLAG_HIDDEN = (1 << 0),
	LV_OBJ_FLAG_CLICKABLE = (1 << 1),
	LV_OBJ_FLAG_CHECKABLE = (1 << 3),
	LV_OBJ_FLAG_SCROLLABLE = (1 << 4),
};

/* Flex flow */
enum {
	LV_FLEX_FLOW_ROW = 0,
	LV_FLEX_FLOW_COLUMN = 1,
};

/* Part selectors */
enum {
	LV_PART_MAIN = 0x000000,
	LV_PART_INDICATOR = 0x010000,
};

/* Palette */
enum {
	LV_PALETTE_BLUE = 6,
};

/* Animation enable */
enum {
	LV_ANIM_OFF = 0,
	LV_ANIM_ON = 1,
};

/* Screen-load animation types */
enum {
	LV_SCR_LOAD_ANIM_NONE = 0,
	LV_SCR_LOAD_ANIM_OVER_LEFT,
	LV_SCR_LOAD_ANIM_OVER_RIGHT,
	LV_SCR_LOAD_ANIM_OVER_TOP,
	LV_SCR_LOAD_ANIM_OVER_BOTTOM,
	LV_SCR_LOAD_ANIM_MOVE_LEFT,
	LV_SCR_LOAD_ANIM_MOVE_RIGHT,
	LV_SCR_LOAD_ANIM_MOVE_TOP,
	LV_SCR_LOAD_ANIM_MOVE_BOTTOM,
	LV_SCR_LOAD_ANIM_FADE_IN,
	LV_SCR_LOAD_ANIM_FADE_OUT,
	LV_SCR_LOAD_ANIM_OUT_LEFT,
	LV_SCR_LOAD_ANIM_OUT_RIGHT,
	LV_SCR_LOAD_ANIM_OUT_TOP,
	LV_SCR_LOAD_ANIM_OUT_BOTTOM,
};

/* Size content sentinel — matches LVGL v9: LV_COORD_SET_SPEC(LV_COORD_MAX) */
#define LV_SIZE_CONTENT 0x3FFFFFFF
#define LV_PCT(x) (x)
#define LV_OPA_COVER 255

/* ── Opaque types ───────────────────────────────────────────────── */

typedef struct lv_group_t lv_group_t;
typedef struct lv_chart_series_t lv_chart_series_t;
typedef struct lv_timer_t lv_timer_t;
typedef struct lv_layer_t lv_layer_t;
typedef struct lv_observer_t lv_observer_t;

typedef struct {
	uint32_t year;
	uint32_t month;
	uint32_t day;
} lv_calendar_date_t;

typedef struct {
	uint32_t dummy[16];
} lv_subject_t;

typedef struct {
	void *var;
	uint32_t dummy[32];
} lv_anim_t;

typedef void (*lv_timer_cb_t)(lv_timer_t *);
typedef void (*lv_anim_exec_xcb_t)(void *, int32_t);
typedef int32_t (*lv_anim_path_cb_t)(const lv_anim_t *);
typedef void (*lv_anim_completed_cb_t)(lv_anim_t *);

typedef struct {
	uint32_t cf;
	uint32_t w;
	uint32_t h;
} lv_image_header_t;

typedef struct {
	lv_image_header_t header;
	uint32_t data_size;
	const uint8_t *data;
} lv_image_dsc_t;

/* ── Object core ─────────────────────────────────────────────────── */

lv_obj_t *lv_obj_create(lv_obj_t *parent);
void lv_obj_delete(lv_obj_t *obj);
void lv_obj_clean(lv_obj_t *obj);
lv_obj_t *lv_obj_get_parent(lv_obj_t *obj);
uint32_t lv_obj_get_child_count(lv_obj_t *obj);
int32_t lv_obj_get_width(lv_obj_t *obj);
int32_t lv_obj_get_height(lv_obj_t *obj);

void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h);
void lv_obj_set_width(lv_obj_t *obj, int32_t w);
void lv_obj_set_height(lv_obj_t *obj, int32_t h);
void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y);
void lv_obj_set_x(lv_obj_t *obj, int32_t x);
void lv_obj_set_y(lv_obj_t *obj, int32_t y);
int32_t lv_obj_get_x(lv_obj_t *obj);
int32_t lv_obj_get_y(lv_obj_t *obj);
void lv_obj_center(lv_obj_t *obj);
void lv_obj_align(lv_obj_t *obj, int32_t align, int32_t x_ofs, int32_t y_ofs);

void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_add_state(lv_obj_t *obj, uint32_t state);
void lv_obj_remove_state(lv_obj_t *obj, uint32_t state);
bool lv_obj_has_state(lv_obj_t *obj, uint32_t state);

void lv_obj_set_user_data(lv_obj_t *obj, void *data);
void *lv_obj_get_user_data(lv_obj_t *obj);

void lv_obj_set_flex_flow(lv_obj_t *obj, uint32_t flow);
void lv_obj_set_flex_align(lv_obj_t *obj, uint32_t main, uint32_t cross, uint32_t track);
void lv_obj_set_flex_grow(lv_obj_t *obj, uint8_t grow);

/* ── Grid ───────────────────────────────────────────────────────── */

void lv_obj_set_grid_dsc_array(lv_obj_t *obj, const int32_t *col_dsc, const int32_t *row_dsc);
void lv_obj_set_grid_cell(lv_obj_t *obj, uint32_t col_align, int32_t col_pos, int32_t col_span,
			  uint32_t row_align, int32_t row_pos, int32_t row_span);

/* ── Events ──────────────────────────────────────────────────────── */

void lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t code, void *user_data);
void *lv_event_get_user_data(lv_event_t *e);
lv_obj_t *lv_event_get_target(lv_event_t *e);

/* ── Inline styles ───────────────────────────────────────────────── */

void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, uint32_t sel);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t opa, uint32_t sel);
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t color, uint32_t sel);
void lv_obj_set_style_border_width(lv_obj_t *obj, int32_t w, uint32_t sel);
void lv_obj_set_style_radius(lv_obj_t *obj, int32_t radius, uint32_t sel);
void lv_obj_set_style_pad_top(lv_obj_t *obj, int32_t p, uint32_t sel);
void lv_obj_set_style_pad_bottom(lv_obj_t *obj, int32_t p, uint32_t sel);
void lv_obj_set_style_pad_left(lv_obj_t *obj, int32_t p, uint32_t sel);
void lv_obj_set_style_pad_right(lv_obj_t *obj, int32_t p, uint32_t sel);
void lv_obj_set_style_pad_row(lv_obj_t *obj, int32_t g, uint32_t sel);
void lv_obj_set_style_pad_column(lv_obj_t *obj, int32_t g, uint32_t sel);
void lv_obj_set_style_pad_all(lv_obj_t *obj, int32_t p, uint32_t sel);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, uint32_t sel);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, uint32_t sel);
void lv_obj_set_style_text_align(lv_obj_t *obj, uint32_t align, uint32_t sel);
void lv_obj_set_style_opa(lv_obj_t *obj, uint8_t opa, uint32_t sel);
void lv_obj_set_style_arc_color(lv_obj_t *obj, lv_color_t color, uint32_t sel);
void lv_obj_set_style_arc_width(lv_obj_t *obj, int32_t w, uint32_t sel);

/* ── Style object ────────────────────────────────────────────────── */

void lv_style_init(lv_style_t *style);
void lv_style_reset(lv_style_t *style);
void lv_style_set_bg_color(lv_style_t *style, lv_color_t color);
void lv_style_set_bg_opa(lv_style_t *style, lv_opa_t opa);
void lv_style_set_radius(lv_style_t *style, int32_t r);
void lv_style_set_border_color(lv_style_t *style, lv_color_t color);
void lv_style_set_border_width(lv_style_t *style, int32_t w);
void lv_style_set_pad_top(lv_style_t *style, int32_t p);
void lv_style_set_pad_bottom(lv_style_t *style, int32_t p);
void lv_style_set_pad_left(lv_style_t *style, int32_t p);
void lv_style_set_pad_right(lv_style_t *style, int32_t p);
void lv_style_set_pad_all(lv_style_t *style, int32_t p);
void lv_style_set_text_color(lv_style_t *style, lv_color_t color);
void lv_style_set_text_font(lv_style_t *style, const lv_font_t *f);

void lv_obj_add_style(lv_obj_t *obj, lv_style_t *style, uint32_t sel);
void lv_obj_remove_style_all(lv_obj_t *obj);

/* ── Color helpers ───────────────────────────────────────────────── */

lv_color_t lv_palette_main(uint32_t p);
lv_color_t lv_palette_lighten(uint32_t p, uint8_t lvl);
lv_color_t lv_palette_darken(uint32_t p, uint8_t lvl);
lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b);
lv_color_t lv_color_white(void);
lv_color_t lv_color_black(void);
lv_color_t lv_color_hex(uint32_t hex);
lv_color_t lv_color_hex3(uint32_t c);

/* ── Label ───────────────────────────────────────────────────────── */

lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_label_set_text(lv_obj_t *obj, const char *text);
void lv_label_set_text_static(lv_obj_t *obj, const char *text);
void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t mode);
void lv_label_bind_text(lv_obj_t *obj, lv_subject_t *subject, const char *fmt);

/* ── Bar ─────────────────────────────────────────────────────────── */

lv_obj_t *lv_bar_create(lv_obj_t *parent);
void lv_bar_set_value(lv_obj_t *obj, int32_t value, int32_t anim);
void lv_bar_set_range(lv_obj_t *obj, int32_t min, int32_t max);

/* ── Button ──────────────────────────────────────────────────────── */

lv_obj_t *lv_button_create(lv_obj_t *parent);

/* ── Slider ──────────────────────────────────────────────────────── */

lv_obj_t *lv_slider_create(lv_obj_t *parent);
void lv_slider_set_value(lv_obj_t *obj, int32_t value, int32_t anim);
void lv_slider_set_range(lv_obj_t *obj, int32_t min, int32_t max);
int32_t lv_slider_get_value(lv_obj_t *obj);
void lv_slider_bind_value(lv_obj_t *obj, lv_subject_t *subject);

/* ── Switch ──────────────────────────────────────────────────────── */

lv_obj_t *lv_switch_create(lv_obj_t *parent);

/* ── Checkbox ────────────────────────────────────────────────────── */

lv_obj_t *lv_checkbox_create(lv_obj_t *parent);
void lv_checkbox_set_text(lv_obj_t *obj, const char *text);
void lv_checkbox_set_text_static(lv_obj_t *obj, const char *text);

/* ── Arc ─────────────────────────────────────────────────────────── */

lv_obj_t *lv_arc_create(lv_obj_t *parent);
void lv_arc_set_value(lv_obj_t *obj, int32_t value);
void lv_arc_set_range(lv_obj_t *obj, int32_t min, int32_t max);
void lv_arc_set_bg_angles(lv_obj_t *obj, uint32_t start, uint32_t end);
void lv_arc_set_angles(lv_obj_t *obj, uint32_t start, uint32_t end);
void lv_arc_set_rotation(lv_obj_t *obj, uint32_t rotation);
int32_t lv_arc_get_value(lv_obj_t *obj);
void lv_arc_bind_value(lv_obj_t *obj, lv_subject_t *subject);

/* ── Image ───────────────────────────────────────────────────────── */

lv_obj_t *lv_image_create(lv_obj_t *parent);
void lv_image_set_src(lv_obj_t *obj, const void *src);
void lv_image_set_rotation(lv_obj_t *obj, int32_t angle);
void lv_image_set_scale(lv_obj_t *obj, uint32_t zoom);
void lv_image_set_pivot(lv_obj_t *obj, int32_t x, int32_t y);

/* ── Msgbox ──────────────────────────────────────────────────────── */

lv_obj_t *lv_msgbox_create(lv_obj_t *parent);
void lv_msgbox_add_title(lv_obj_t *obj, const char *title);
void lv_msgbox_add_text(lv_obj_t *obj, const char *text);
void lv_msgbox_add_close_button(lv_obj_t *obj);
lv_obj_t *lv_msgbox_add_footer_button(lv_obj_t *obj, const char *text);
lv_obj_t *lv_msgbox_get_content(lv_obj_t *obj);
lv_obj_t *lv_msgbox_get_header(lv_obj_t *obj);
lv_obj_t *lv_msgbox_get_footer(lv_obj_t *obj);
void lv_msgbox_close(lv_obj_t *obj);

/* ── Spinner ─────────────────────────────────────────────────────── */

lv_obj_t *lv_spinner_create(lv_obj_t *parent);
void lv_spinner_set_anim_params(lv_obj_t *obj, uint32_t time_ms, uint32_t angle_deg);

/* ── Led ─────────────────────────────────────────────────────────── */

lv_obj_t *lv_led_create(lv_obj_t *parent);
void lv_led_set_color(lv_obj_t *obj, lv_color_t color);
void lv_led_set_brightness(lv_obj_t *obj, uint8_t bright);
void lv_led_on(lv_obj_t *obj);
void lv_led_off(lv_obj_t *obj);
void lv_led_toggle(lv_obj_t *obj);
uint8_t lv_led_get_brightness(lv_obj_t *obj);

/* ── Textarea ────────────────────────────────────────────────────── */

lv_obj_t *lv_textarea_create(lv_obj_t *parent);
void lv_textarea_set_text(lv_obj_t *obj, const char *text);
void lv_textarea_add_text(lv_obj_t *obj, const char *text);
void lv_textarea_set_placeholder_text(lv_obj_t *obj, const char *text);
void lv_textarea_set_one_line(lv_obj_t *obj, bool en);
void lv_textarea_set_password_mode(lv_obj_t *obj, bool en);
void lv_textarea_set_max_length(lv_obj_t *obj, uint32_t max_len);
void lv_textarea_set_accepted_chars(lv_obj_t *obj, const char *list);
void lv_textarea_set_cursor_pos(lv_obj_t *obj, int32_t pos);
void lv_textarea_set_cursor_click_pos(lv_obj_t *obj, bool en);
const char *lv_textarea_get_text(lv_obj_t *obj);
uint32_t lv_textarea_get_cursor_pos(lv_obj_t *obj);
bool lv_textarea_get_password_mode(lv_obj_t *obj);
bool lv_textarea_get_one_line(lv_obj_t *obj);
void lv_textarea_add_char(lv_obj_t *obj, uint32_t c);
void lv_textarea_delete_char(lv_obj_t *obj);

/* ── Dropdown ────────────────────────────────────────────────────── */

lv_obj_t *lv_dropdown_create(lv_obj_t *parent);
void lv_dropdown_set_options(lv_obj_t *obj, const char *opts);
void lv_dropdown_set_options_static(lv_obj_t *obj, const char *opts);
void lv_dropdown_add_option(lv_obj_t *obj, const char *opt, uint32_t pos);
void lv_dropdown_clear_options(lv_obj_t *obj);
void lv_dropdown_set_selected(lv_obj_t *obj, uint32_t sel);
uint32_t lv_dropdown_get_selected(lv_obj_t *obj);
uint32_t lv_dropdown_get_option_count(lv_obj_t *obj);
void lv_dropdown_get_selected_str(lv_obj_t *obj, char *buf, uint32_t buf_size);
void lv_dropdown_set_dir(lv_obj_t *obj, lv_dir_t dir);
void lv_dropdown_set_symbol(lv_obj_t *obj, const void *symbol);
bool lv_dropdown_is_open(lv_obj_t *obj);
void lv_dropdown_open(lv_obj_t *obj);
void lv_dropdown_close(lv_obj_t *obj);
void lv_dropdown_bind_value(lv_obj_t *obj, lv_subject_t *subject);

/* ── Roller ──────────────────────────────────────────────────────── */

lv_obj_t *lv_roller_create(lv_obj_t *parent);
void lv_roller_set_options(lv_obj_t *obj, const char *opts, uint32_t mode);
void lv_roller_set_selected(lv_obj_t *obj, uint32_t sel, int32_t anim);
void lv_roller_set_visible_row_count(lv_obj_t *obj, uint32_t rows);
uint32_t lv_roller_get_selected(lv_obj_t *obj);
uint32_t lv_roller_get_option_count(lv_obj_t *obj);
void lv_roller_get_selected_str(lv_obj_t *obj, char *buf, uint32_t buf_size);
void lv_roller_bind_value(lv_obj_t *obj, lv_subject_t *subject);

/* ── Spinbox ─────────────────────────────────────────────────────── */

lv_obj_t *lv_spinbox_create(lv_obj_t *parent);
void lv_spinbox_set_value(lv_obj_t *obj, int32_t val);
void lv_spinbox_set_range(lv_obj_t *obj, int32_t min, int32_t max);
void lv_spinbox_set_step(lv_obj_t *obj, uint32_t step);
void lv_spinbox_set_digit_format(lv_obj_t *obj, uint32_t digit_count, uint32_t sep_pos);
void lv_spinbox_set_rollover(lv_obj_t *obj, bool en);
void lv_spinbox_set_cursor_pos(lv_obj_t *obj, uint32_t pos);
int32_t lv_spinbox_get_value(lv_obj_t *obj);
int32_t lv_spinbox_get_step(lv_obj_t *obj);
void lv_spinbox_increment(lv_obj_t *obj);
void lv_spinbox_decrement(lv_obj_t *obj);

/* ── Keyboard ────────────────────────────────────────────────────── */

lv_obj_t *lv_keyboard_create(lv_obj_t *parent);
void lv_keyboard_set_textarea(lv_obj_t *kb, lv_obj_t *ta);
void lv_keyboard_set_mode(lv_obj_t *kb, uint32_t mode);
void lv_keyboard_set_popovers(lv_obj_t *kb, bool en);
lv_obj_t *lv_keyboard_get_textarea(lv_obj_t *kb);

/* ── Chart ───────────────────────────────────────────────────────── */

lv_obj_t *lv_chart_create(lv_obj_t *parent);
void lv_chart_set_type(lv_obj_t *obj, uint32_t type);
void lv_chart_set_point_count(lv_obj_t *obj, uint32_t count);
void lv_chart_set_axis_range(lv_obj_t *obj, uint32_t axis, int32_t min, int32_t max);
void lv_chart_set_update_mode(lv_obj_t *obj, uint32_t mode);
void lv_chart_set_div_line_count(lv_obj_t *obj, uint32_t hdiv, uint32_t vdiv);
lv_chart_series_t *lv_chart_add_series(lv_obj_t *obj, lv_color_t color, uint32_t axis);
void lv_chart_remove_series(lv_obj_t *obj, lv_chart_series_t *series);
void lv_chart_set_next_value(lv_obj_t *obj, lv_chart_series_t *series, int32_t value);
void lv_chart_set_series_value_by_id(lv_obj_t *obj, lv_chart_series_t *series, uint32_t id,
				     int32_t value);

/* ── Table ───────────────────────────────────────────────────────── */

lv_obj_t *lv_table_create(lv_obj_t *parent);
void lv_table_set_cell_value(lv_obj_t *obj, uint32_t row, uint32_t col, const char *txt);
void lv_table_set_row_count(lv_obj_t *obj, uint32_t cnt);
void lv_table_set_column_count(lv_obj_t *obj, uint32_t cnt);
void lv_table_set_column_width(lv_obj_t *obj, uint32_t col, int32_t w);
const char *lv_table_get_cell_value(lv_obj_t *obj, uint32_t row, uint32_t col);
uint32_t lv_table_get_row_count(lv_obj_t *obj);
uint32_t lv_table_get_column_count(lv_obj_t *obj);
int32_t lv_table_get_column_width(lv_obj_t *obj, uint32_t col);

/* ── Tabview ─────────────────────────────────────────────────────── */

lv_obj_t *lv_tabview_create(lv_obj_t *parent);
lv_obj_t *lv_tabview_add_tab(lv_obj_t *tv, const char *name);
void lv_tabview_rename_tab(lv_obj_t *tv, uint32_t idx, const char *name);
void lv_tabview_set_active(lv_obj_t *tv, uint32_t idx, int32_t anim);
void lv_tabview_set_tab_bar_position(lv_obj_t *tv, lv_dir_t dir);
void lv_tabview_set_tab_bar_size(lv_obj_t *tv, int32_t size);
uint32_t lv_tabview_get_tab_count(lv_obj_t *tv);
uint32_t lv_tabview_get_tab_active(lv_obj_t *tv);
lv_obj_t *lv_tabview_get_content(lv_obj_t *tv);

/* ── List ────────────────────────────────────────────────────────── */

lv_obj_t *lv_list_create(lv_obj_t *parent);
lv_obj_t *lv_list_add_text(lv_obj_t *list, const char *text);
lv_obj_t *lv_list_add_button(lv_obj_t *list, const void *icon, const char *text);
const char *lv_list_get_button_text(lv_obj_t *list, lv_obj_t *btn);

/* ── Canvas ──────────────────────────────────────────────────────── */

lv_obj_t *lv_canvas_create(lv_obj_t *parent);
void lv_canvas_set_buffer(lv_obj_t *obj, void *buf, int32_t w, int32_t h, lv_color_format_t cf);
void lv_canvas_fill_bg(lv_obj_t *obj, lv_color_t color, uint8_t opa);
void lv_canvas_set_px(lv_obj_t *obj, int32_t x, int32_t y, lv_color_t color, uint8_t opa);
void lv_canvas_init_layer(lv_obj_t *obj, lv_layer_t *layer);
void lv_canvas_finish_layer(lv_obj_t *obj, lv_layer_t *layer);

/* ── Calendar ────────────────────────────────────────────────────── */

lv_obj_t *lv_calendar_create(lv_obj_t *parent);
void lv_calendar_set_today_date(lv_obj_t *obj, uint32_t year, uint32_t month, uint32_t day);
void lv_calendar_set_month_shown(lv_obj_t *obj, uint32_t year, uint32_t month);
void lv_calendar_set_highlighted_dates(lv_obj_t *obj, lv_calendar_date_t dates[], uint32_t cnt);
uint32_t lv_calendar_get_pressed_date(lv_obj_t *obj, lv_calendar_date_t *date);
lv_obj_t *lv_calendar_add_header_arrow(lv_obj_t *parent);
lv_obj_t *lv_calendar_add_header_dropdown(lv_obj_t *parent);

/* ── Screen ──────────────────────────────────────────────────────── */

lv_obj_t *lv_screen_active(void);
void lv_screen_load(lv_obj_t *scr);
void lv_screen_load_anim(lv_obj_t *scr, lv_screen_load_anim_t anim, uint32_t time_ms,
			 uint32_t delay_ms, bool auto_del);

/* ── Group (focus navigation) ────────────────────────────────────── */

lv_group_t *lv_group_create(void);
void lv_group_delete(lv_group_t *group);
void lv_group_set_default(lv_group_t *group);
lv_group_t *lv_group_get_default(void);
void lv_group_add_obj(lv_group_t *group, lv_obj_t *obj);
void lv_group_remove_obj(lv_obj_t *obj);
void lv_group_remove_all_objs(lv_group_t *group);
void lv_group_focus_obj(lv_obj_t *obj);
void lv_group_focus_next(lv_group_t *group);
void lv_group_focus_prev(lv_group_t *group);
void lv_group_focus_freeze(lv_group_t *group, bool en);
lv_obj_t *lv_group_get_focused(lv_group_t *group);
void lv_group_set_editing(lv_group_t *group, bool en);
bool lv_group_get_editing(lv_group_t *group);
uint32_t lv_group_get_obj_count(lv_group_t *group);

/* ── Subject / Observer (reactive state) ─────────────────────────── */

void lv_subject_init_int(lv_subject_t *subject, int32_t value);
void lv_subject_set_int(lv_subject_t *subject, int32_t value);
int32_t lv_subject_get_int(lv_subject_t *subject);
void lv_subject_deinit(lv_subject_t *subject);
typedef void (*lv_observer_cb_t)(lv_observer_t *observer, lv_subject_t *subject);
lv_observer_t *lv_subject_add_observer_obj(lv_subject_t *subject, lv_observer_cb_t cb,
					   lv_obj_t *obj, void *user_data);
void lv_subject_notify(lv_subject_t *subject);
void lv_observer_remove(lv_observer_t *observer);

void lv_arc_bind_value(lv_obj_t *obj, lv_subject_t *subject);

/* ── Animation ───────────────────────────────────────────────────── */

void lv_anim_init(lv_anim_t *a);
void lv_anim_set_var(lv_anim_t *a, void *var);
void lv_anim_set_values(lv_anim_t *a, int32_t start, int32_t end);
void lv_anim_set_duration(lv_anim_t *a, uint32_t duration);
void lv_anim_set_delay(lv_anim_t *a, uint32_t delay);
void lv_anim_set_exec_cb(lv_anim_t *a, lv_anim_exec_xcb_t exec_cb);
void lv_anim_set_path_cb(lv_anim_t *a, lv_anim_path_cb_t path_cb);
void lv_anim_set_repeat_count(lv_anim_t *a, uint32_t cnt);
void lv_anim_set_repeat_delay(lv_anim_t *a, uint32_t delay);
void lv_anim_set_reverse_duration(lv_anim_t *a, uint32_t duration);
void lv_anim_set_reverse_delay(lv_anim_t *a, uint32_t delay);
void lv_anim_set_completed_cb(lv_anim_t *a, lv_anim_completed_cb_t cb);
void lv_anim_start(const lv_anim_t *a);
bool lv_anim_delete(void *var, lv_anim_exec_xcb_t exec_cb);

/* Animation path functions */
int32_t lv_anim_path_linear(const lv_anim_t *a);
int32_t lv_anim_path_ease_in(const lv_anim_t *a);
int32_t lv_anim_path_ease_out(const lv_anim_t *a);
int32_t lv_anim_path_ease_in_out(const lv_anim_t *a);
int32_t lv_anim_path_overshoot(const lv_anim_t *a);
int32_t lv_anim_path_bounce(const lv_anim_t *a);
int32_t lv_anim_path_step(const lv_anim_t *a);

/* ── Timer ───────────────────────────────────────────────────────── */

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data);
void lv_timer_delete(lv_timer_t *timer);
void lv_timer_pause(lv_timer_t *timer);
void lv_timer_resume(lv_timer_t *timer);
void lv_timer_set_period(lv_timer_t *timer, uint32_t period);
void lv_timer_set_repeat_count(lv_timer_t *timer, int32_t cnt);
void lv_timer_reset(lv_timer_t *timer);
void lv_timer_ready(lv_timer_t *timer);
void *lv_timer_get_user_data(lv_timer_t *timer);

/* ── Fonts ───────────────────────────────────────────────────────── */

extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_16;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t lv_font_montserrat_24;
extern const lv_font_t lv_font_montserrat_32;
extern const lv_font_t lv_font_montserrat_48;

/* ── Additional stubs required by the Rust lvgl binding ─────────────────── */

typedef struct {
	int32_t x;
	int32_t y;
} lv_point_t;

typedef struct lv_display_t lv_display_t;
typedef struct lv_scale_section_t lv_scale_section_t;
typedef uint32_t lv_scale_mode_t;
typedef uint32_t lv_image_align_t;

/* Object layout / scrolling */
void lv_obj_set_layout(lv_obj_t *obj, uint32_t layout);
void lv_obj_update_layout(const lv_obj_t *obj);
int32_t lv_obj_get_content_width(const lv_obj_t *obj);
int32_t lv_obj_get_scroll_bottom(lv_obj_t *obj);
void lv_obj_scroll_to_y(lv_obj_t *obj, int32_t y, lv_anim_enable_t anim_en);
lv_obj_t *lv_obj_get_child(const lv_obj_t *obj, int32_t id);

/* Style setters used by the Rust binding */
void lv_obj_set_style_translate_y(lv_obj_t *obj, int32_t value, lv_state_t state);
void lv_obj_set_style_margin_top(lv_obj_t *obj, int32_t value, lv_state_t state);
void lv_obj_set_style_margin_bottom(lv_obj_t *obj, int32_t value, lv_state_t state);
void lv_obj_set_style_margin_left(lv_obj_t *obj, int32_t value, lv_state_t state);
void lv_obj_set_style_margin_right(lv_obj_t *obj, int32_t value, lv_state_t state);
void lv_obj_set_style_max_height(lv_obj_t *obj, int32_t value, lv_state_t state);
void lv_obj_set_style_arc_opa(lv_obj_t *obj, lv_opa_t value, lv_state_t state);
void lv_obj_set_style_arc_rounded(lv_obj_t *obj, bool value, lv_state_t state);
void lv_obj_set_style_opa_layered(lv_obj_t *obj, lv_opa_t value, lv_state_t state);
void lv_style_set_arc_color(lv_style_t *style, lv_color_t value);
void lv_style_set_arc_width(lv_style_t *style, int32_t value);

/* Event accessors */
lv_obj_t *lv_event_get_current_target(lv_event_t *e);
lv_event_code_t lv_event_get_code(lv_event_t *e);
void *lv_event_get_param(lv_event_t *e);

/* Display / layer introspection */
int32_t lv_display_get_horizontal_resolution(const lv_display_t *disp);
int32_t lv_display_get_vertical_resolution(const lv_display_t *disp);
int32_t lv_display_get_dpi(const lv_display_t *disp);
lv_obj_t *lv_layer_top(void);

/* Text metrics */
void lv_text_get_size(lv_point_t *size_res, const char *text, const lv_font_t *font,
		      int32_t letter_space, int32_t line_space, int32_t max_width, uint32_t flag);

/* Animation extras */
uint32_t lv_anim_speed(uint32_t speed);
void lv_anim_set_user_data(lv_anim_t *a, void *user_data);
void *lv_anim_get_user_data(const lv_anim_t *a);
void lv_anim_set_custom_exec_cb(lv_anim_t *a, void (*exec_cb)(lv_anim_t *, int32_t));

/* Image / scale widgets */
void lv_image_set_inner_align(lv_obj_t *img, lv_image_align_t align);

lv_obj_t *lv_scale_create(lv_obj_t *parent);
void lv_scale_set_mode(lv_obj_t *obj, lv_scale_mode_t mode);
void lv_scale_set_range(lv_obj_t *obj, int32_t min, int32_t max);
void lv_scale_set_total_tick_count(lv_obj_t *obj, uint32_t total);
void lv_scale_set_major_tick_every(lv_obj_t *obj, uint32_t every);
void lv_scale_set_angle_range(lv_obj_t *obj, uint32_t angle);
void lv_scale_set_rotation(lv_obj_t *obj, int32_t rot);
lv_scale_section_t *lv_scale_add_section(lv_obj_t *obj);
void lv_scale_section_set_range(lv_scale_section_t *s, int32_t min, int32_t max);
void lv_scale_section_set_style(lv_scale_section_t *s, uint32_t part, lv_style_t *style);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_H */
