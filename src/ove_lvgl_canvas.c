/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/lvgl_internal.h"

#ifdef CONFIG_OVE_LVGL
#if defined(__ZEPHYR__)
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif

#if !defined(OVE_LVGL_STUB) && defined(LV_USE_CANVAS) && LV_USE_CANVAS
#include <limits.h>
#if defined(__ZEPHYR__)
#include <src/core/lv_obj_class_private.h>
#include <src/draw/lv_draw_buf_private.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <src/widgets/canvas/lv_canvas_private.h>
#else
#include <lvgl/src/core/lv_obj_class_private.h>
#include <lvgl/src/draw/lv_draw_buf_private.h>
#include <lvgl/src/misc/cache/instance/lv_image_cache.h>
#include <lvgl/src/widgets/canvas/lv_canvas_private.h>
#endif

struct canvas_pixels {
	lv_draw_buf_t buf;
	lv_draw_buf_handlers_t handlers;
};

struct owned_canvas {
	lv_canvas_t base;
	struct canvas_pixels *pixels;
};

struct canvas_layout {
	uint32_t row;
	uint32_t stride;
	uint32_t palette;
	uint32_t bytes;
	size_t packed;
};

static void canvas_destructor(const lv_obj_class_t *class_p, lv_obj_t *obj);
static const lv_obj_class_t owned_canvas_class = {
	.base_class = &lv_canvas_class,
	.destructor_cb = canvas_destructor,
	.instance_size = sizeof(struct owned_canvas),
	.name = "ove_canvas",
};

static struct owned_canvas *canvas_get(void *obj, int attaching)
{
	if (!lv_is_initialized() || !obj || !lv_obj_is_valid(obj))
		return NULL;
	lv_obj_t *widget = obj;
	if (!lv_obj_check_type(widget, &owned_canvas_class) || (attaching && widget->is_deleting))
		return NULL;
	return obj;
}

static uint32_t pixel_alignment(uint32_t format)
{
	if (format == LV_COLOR_FORMAT_RGB565)
		return 2;
	if (format == LV_COLOR_FORMAT_I1 || format == LV_COLOR_FORMAT_ARGB8888 ||
	    format == LV_COLOR_FORMAT_XRGB8888)
		return 4;
	return 1;
}

static int canvas_layout(int32_t w, int32_t h, uint32_t format, struct canvas_layout *layout)
{
	if (w <= 0 || h <= 0 || w > UINT16_MAX || h > UINT16_MAX)
		return OVE_ERR_INVALID_PARAM;
	uint32_t bits;
	uint32_t palette = 0;
	switch (format) {
	case LV_COLOR_FORMAT_I1:
		bits = 1;
		palette = 8;
		break;
	case LV_COLOR_FORMAT_A8:
		bits = 8;
		break;
	case LV_COLOR_FORMAT_RGB565:
		bits = 16;
		break;
	case LV_COLOR_FORMAT_RGB888:
		bits = 24;
		break;
	case LV_COLOR_FORMAT_ARGB8888:
	case LV_COLOR_FORMAT_XRGB8888:
		bits = 32;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}
	uint32_t row = ((uint32_t)w * bits + 7) / 8;
	uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, (lv_color_format_t)format);
	if (row > UINT16_MAX || stride < row || stride > UINT16_MAX)
		return OVE_ERR_INVALID_PARAM;
	if (format != LV_COLOR_FORMAT_I1 && stride % pixel_alignment(format) != 0)
		return OVE_ERR_INVALID_PARAM;
	if ((uint32_t)h > (UINT32_MAX - palette) / stride || (size_t)h > (SIZE_MAX - palette) / row)
		return OVE_ERR_INVALID_PARAM;
	uint32_t bytes = stride * (uint32_t)h + palette;
	if ((size_t)bytes > SIZE_MAX - (LV_DRAW_BUF_ALIGN - 1))
		return OVE_ERR_INVALID_PARAM;
	*layout = (struct canvas_layout){.row = row,
					 .stride = stride,
					 .palette = palette,
					 .bytes = bytes,
					 .packed = (size_t)row * (size_t)h + palette};
	return OVE_OK;
}

static void pixels_free(struct canvas_pixels *pixels)
{
	if (!pixels)
		return;
	if (pixels->buf.unaligned_data)
		pixels->handlers.buf_free_cb(pixels->buf.unaligned_data);
	lv_free(pixels);
}

static void canvas_destructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
	(void)class_p;
	struct owned_canvas *canvas = (struct owned_canvas *)obj;
	struct canvas_pixels *pixels = canvas->pixels;
	lv_draw_buf_t *draw_buf = canvas->base.draw_buf;
	/* All object/child DELETE callbacks have finished. Detach before cache
	 * callbacks, keeping both descriptor and bytes live until eviction ends. */
	canvas->pixels = NULL;
	canvas->base.draw_buf = NULL;
	if (canvas->base.img.src_type == LV_IMAGE_SRC_VARIABLE &&
	    (canvas->base.img.src == draw_buf ||
	     (pixels && canvas->base.img.src == &pixels->buf))) {
		canvas->base.img.src = NULL;
		canvas->base.img.src_type = LV_IMAGE_SRC_UNKNOWN;
	}
	if (draw_buf)
		lv_image_cache_drop(draw_buf);
	if (pixels && draw_buf != &pixels->buf)
		lv_image_cache_drop(&pixels->buf);
	pixels_free(pixels);
}

void *ove_lvgl_canvas_create(void *parent)
{
	if (!lv_is_initialized() || !parent || !lv_obj_is_valid(parent) ||
	    ((lv_obj_t *)parent)->is_deleting)
		return NULL;
	lv_obj_t *obj = lv_obj_class_create_obj(&owned_canvas_class, parent);
	lv_obj_class_init_obj(obj);
	return obj;
}

int ove_lvgl_canvas_copy(void *obj, const uint8_t *data, size_t size, int32_t w, int32_t h,
			 uint32_t format)
{
	struct owned_canvas *canvas = canvas_get(obj, 1);
	if (!canvas)
		return OVE_ERR_BAD_HANDLE;
	struct canvas_layout layout;
	int rc = canvas_layout(w, h, format, &layout);
	if (rc != OVE_OK || (data ? size != layout.packed : size != 0))
		return rc != OVE_OK ? rc : OVE_ERR_INVALID_PARAM;
	struct canvas_pixels *pixels = lv_malloc_zeroed(sizeof(*pixels));
	if (!pixels)
		return OVE_ERR_NO_MEMORY;
	pixels->handlers = *lv_draw_buf_get_handlers();
	if (!pixels->handlers.buf_malloc_cb || !pixels->handlers.buf_free_cb) {
		pixels_free(pixels);
		return OVE_ERR_NOT_SUPPORTED;
	}
	void *allocation = pixels->handlers.buf_malloc_cb(layout.bytes, (lv_color_format_t)format);
	if (!allocation) {
		pixels_free(pixels);
		return OVE_ERR_NO_MEMORY;
	}
	pixels->buf.unaligned_data = allocation;
	uint8_t *aligned =
		lv_draw_buf_align_ex(&pixels->handlers, allocation, (lv_color_format_t)format);
	if (!aligned || (uintptr_t)aligned < (uintptr_t)allocation ||
	    (uintptr_t)aligned - (uintptr_t)allocation >= LV_DRAW_BUF_ALIGN ||
	    (uintptr_t)aligned % LV_DRAW_BUF_ALIGN != 0 ||
	    (uintptr_t)aligned % pixel_alignment(format) != 0) {
		pixels_free(pixels);
		return OVE_ERR_INVALID_PARAM;
	}
	lv_memzero(aligned, layout.bytes);
	if (lv_draw_buf_init(&pixels->buf, (uint32_t)w, (uint32_t)h, (lv_color_format_t)format,
			     layout.stride, aligned, layout.bytes) != LV_RESULT_OK) {
		/* init clears the descriptor, including the allocation pointer. */
		pixels->buf.unaligned_data = allocation;
		pixels_free(pixels);
		return OVE_ERR_INVALID_PARAM;
	}
	pixels->buf.unaligned_data = allocation;
	pixels->buf.handlers = &pixels->handlers;
	/* Keep source flags zero: decoder alpha normalization must use a copy. */
	if (data) {
		lv_memcpy(aligned, data, layout.palette);
		for (uint32_t y = 0; y < (uint32_t)h; y++)
			lv_memcpy(aligned + layout.palette + y * layout.stride,
				  data + layout.palette + (size_t)y * layout.row, layout.row);
	} else if (format == LV_COLOR_FORMAT_I1) {
		lv_draw_buf_set_palette(&pixels->buf, 0, lv_color_to_32(lv_color_black(), 255));
		lv_draw_buf_set_palette(&pixels->buf, 1, lv_color_to_32(lv_color_white(), 255));
	}
	struct canvas_pixels *old = canvas->pixels;
	canvas->pixels = pixels;
	lv_canvas_set_draw_buf(obj, &pixels->buf);
	/* Publication is complete; no instance access follows native notification.
	 * Raw display/draw callbacks follow the upstream non-deleting contract. */
	pixels_free(old);
	return OVE_OK;
}

int ove_lvgl_canvas_raw(void *obj, void *data, int32_t w, int32_t h, uint32_t format)
{
	struct owned_canvas *canvas = canvas_get(obj, 1);
	if (!canvas)
		return OVE_ERR_BAD_HANDLE;
	struct canvas_layout layout;
	int rc = canvas_layout(w, h, format, &layout);
	if (rc != OVE_OK || !data || (uintptr_t)data % LV_DRAW_BUF_ALIGN != 0 ||
	    (uintptr_t)data % pixel_alignment(format) != 0)
		return rc != OVE_OK ? rc : OVE_ERR_INVALID_PARAM;
	struct canvas_pixels *old = canvas->pixels;
	lv_draw_buf_t *draw_buf = &canvas->base.static_buf;
	/* Evict an earlier borrowed descriptor before reusing its identity. */
	if (canvas->base.draw_buf == draw_buf)
		lv_image_cache_drop(draw_buf);
	if (lv_draw_buf_init(draw_buf, (uint32_t)w, (uint32_t)h, (lv_color_format_t)format,
			     layout.stride, data, layout.bytes) != LV_RESULT_OK)
		return OVE_ERR_INVALID_PARAM;
	canvas->pixels = NULL;
	lv_canvas_set_draw_buf(obj, draw_buf);
	pixels_free(old);
	return OVE_OK;
}

void ove_lvgl_canvas_fill(void *obj, uint32_t rgb, uint8_t opacity)
{
	struct owned_canvas *canvas = canvas_get(obj, 0);
	if (!canvas)
		return;
	lv_draw_buf_t *draw_buf = canvas->base.draw_buf;
	lv_canvas_fill_bg(obj, lv_color_hex(rgb), opacity);
	if (draw_buf)
		lv_image_cache_drop(draw_buf);
}

void ove_lvgl_canvas_pixel(void *obj, int32_t x, int32_t y, uint32_t rgb)
{
	struct owned_canvas *canvas = canvas_get(obj, 0);
	if (!canvas)
		return;
	lv_draw_buf_t *draw_buf = canvas->base.draw_buf;
	lv_canvas_set_px(obj, x, y, lv_color_hex(rgb), 255);
	if (draw_buf)
		lv_image_cache_drop(draw_buf);
}

void ove_lvgl_canvas_finish(void *obj, void *layer)
{
	struct owned_canvas *canvas = canvas_get(obj, 0);
	if (!canvas)
		return;
	lv_draw_buf_t *draw_buf = canvas->base.draw_buf;
	lv_canvas_finish_layer(obj, layer);
	if (draw_buf)
		lv_image_cache_drop(draw_buf);
}

#else
void *ove_lvgl_canvas_create(void *parent)
{
	(void)parent;
	return NULL;
}
int ove_lvgl_canvas_copy(void *obj, const uint8_t *data, size_t size, int32_t w, int32_t h,
			 uint32_t format)
{
	(void)obj;
	(void)data;
	(void)size;
	(void)w;
	(void)h;
	(void)format;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_lvgl_canvas_raw(void *obj, void *data, int32_t w, int32_t h, uint32_t format)
{
	return ove_lvgl_canvas_copy(obj, data, 0, w, h, format);
}
void ove_lvgl_canvas_fill(void *obj, uint32_t rgb, uint8_t opacity)
{
	(void)obj;
	(void)rgb;
	(void)opacity;
}
void ove_lvgl_canvas_pixel(void *obj, int32_t x, int32_t y, uint32_t rgb)
{
	(void)obj;
	(void)x;
	(void)y;
	(void)rgb;
}
void ove_lvgl_canvas_finish(void *obj, void *layer)
{
	(void)obj;
	(void)layer;
}
#endif
#endif
