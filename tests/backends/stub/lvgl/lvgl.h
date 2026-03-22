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
typedef uint32_t lv_anim_enable_t;
typedef uint32_t lv_label_long_mode_t;
typedef void lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *e);
typedef uint32_t lv_event_code_t;

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
    LV_OBJ_FLAG_HIDDEN     = (1 << 0),
    LV_OBJ_FLAG_CLICKABLE  = (1 << 1),
    LV_OBJ_FLAG_SCROLLABLE = (1 << 4),
};

/* Flex flow */
enum {
    LV_FLEX_FLOW_ROW    = 0,
    LV_FLEX_FLOW_COLUMN = 1,
};

/* Part selectors */
enum {
    LV_PART_MAIN      = 0x000000,
    LV_PART_INDICATOR = 0x010000,
};

/* Palette */
enum {
    LV_PALETTE_BLUE = 6,
};

/* Animation enable */
enum {
    LV_ANIM_OFF = 0,
    LV_ANIM_ON  = 1,
};

/* Size content sentinel — matches LVGL v9: LV_COORD_SET_SPEC(LV_COORD_MAX) */
#define LV_SIZE_CONTENT 0x3FFFFFFF
#define LV_PCT(x) (x)
#define LV_OPA_COVER 255

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
void lv_obj_center(lv_obj_t *obj);
void lv_obj_align(lv_obj_t *obj, int32_t align, int32_t x_ofs, int32_t y_ofs);

void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_remove_flag(lv_obj_t *obj, uint32_t flag);
void lv_obj_add_state(lv_obj_t *obj, uint32_t state);
void lv_obj_remove_state(lv_obj_t *obj, uint32_t state);

void lv_obj_set_user_data(lv_obj_t *obj, void *data);
void *lv_obj_get_user_data(lv_obj_t *obj);

void lv_obj_set_flex_flow(lv_obj_t *obj, uint32_t flow);

/* ── Events ──────────────────────────────────────────────────────── */

void lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb,
                         lv_event_code_t code, void *user_data);
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
lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b);
lv_color_t lv_color_white(void);
lv_color_t lv_color_black(void);
lv_color_t lv_color_hex(uint32_t hex);

/* ── Label ───────────────────────────────────────────────────────── */

lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_label_set_text(lv_obj_t *obj, const char *text);
void lv_label_set_text_static(lv_obj_t *obj, const char *text);
void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t mode);

/* ── Bar ─────────────────────────────────────────────────────────── */

lv_obj_t *lv_bar_create(lv_obj_t *parent);
void lv_bar_set_value(lv_obj_t *obj, int32_t value, int32_t anim);
void lv_bar_set_range(lv_obj_t *obj, int32_t min, int32_t max);

/* ── Screen ──────────────────────────────────────────────────────── */

lv_obj_t *lv_screen_active(void);

/* ── Fonts ───────────────────────────────────────────────────────── */

extern const lv_font_t lv_font_montserrat_32;
extern const lv_font_t lv_font_montserrat_14;

#ifdef __cplusplus
}
#endif

#endif /* LVGL_H */
