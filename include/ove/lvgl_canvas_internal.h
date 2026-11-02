/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OVE_LVGL_CANVAS_INTERNAL_H
#define OVE_LVGL_CANVAS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private Rust binding support. Requires initialized, externally locked LVGL.
 * Native display/draw callbacks retain their normal LVGL reentry constraints.
 * Widget construction retains upstream allocation/assertion behavior. */
void *ove_lvgl_canvas_create(void *parent);
/* NULL data with zero size requests initialized blank storage. Otherwise data
 * contains tightly packed rows, preceded by the I1 palette where applicable.
 * Copies synchronously; failure preserves the previous canvas contents. */
int ove_lvgl_canvas_copy(void *obj, const uint8_t *data, size_t size, int32_t w, int32_t h,
			 uint32_t format);
/* Unsafe borrowed-buffer path: caller provides initialized, aligned storage
 * including the native row stride and palette, retained until replacement or
 * native destruction returns. There must be no concurrent Rust references. */
int ove_lvgl_canvas_raw(void *obj, void *data, int32_t w, int32_t h, uint32_t format);
void ove_lvgl_canvas_fill(void *obj, uint32_t rgb, uint8_t opacity);
void ove_lvgl_canvas_pixel(void *obj, int32_t x, int32_t y, uint32_t rgb);
/* Unsafe layer storage/tasks obey the native paired init/finish lifetime. */
void ove_lvgl_canvas_finish(void *obj, void *layer);

#ifdef __cplusplus
}
#endif
#endif
