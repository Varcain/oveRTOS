/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */
#ifndef OVE_LVGL_TEXT_INTERNAL_H
#define OVE_LVGL_TEXT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private Rust binding support. Calls require initialized, locked LVGL. */
typedef struct ove_lvgl_text_watch ove_lvgl_text_watch_t;
typedef void (*ove_lvgl_text_release_fn)(void *data);
typedef int (*ove_lvgl_text_render_fn)(void *obj, uint32_t cookie, int32_t value, void *context);
typedef struct {
	ove_lvgl_text_render_fn render;
	uint32_t cookie;
	int status;
} ove_lvgl_text_binding_t;

void *ove_lvgl_text_alloc(size_t size);
void *ove_lvgl_text_realloc(void *ptr, size_t size);
void ove_lvgl_text_free(void *ptr);
ove_lvgl_text_watch_t *ove_lvgl_text_watch(void *obj, void *data, ove_lvgl_text_release_fn release);
int ove_lvgl_text_watch_valid(ove_lvgl_text_watch_t *watch);
void ove_lvgl_text_watch_end(ove_lvgl_text_watch_t *watch);
int ove_lvgl_text_static(void *obj, const char *text, void *data, ove_lvgl_text_release_fn release);
void ove_lvgl_text_set(void *obj, const char *text);
int ove_lvgl_text_bind(void *obj, void *subject, ove_lvgl_text_binding_t *context);
int ove_lvgl_text_current(void *obj, uint32_t cookie, int32_t value);
int ove_lvgl_text_status(void *obj);
int ove_lvgl_text_publish(void *obj, uint32_t cookie, int32_t value, const char *text);

#ifdef __cplusplus
}
#endif

#endif
