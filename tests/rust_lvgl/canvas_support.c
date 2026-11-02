/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/lvgl_internal.h"
#include <lvgl/lvgl.h>
#include <lvgl/src/core/lv_obj_class_private.h>
#include <lvgl/src/draw/lv_draw_buf_private.h>
#include <lvgl/src/draw/lv_draw_private.h>
#include <lvgl/src/draw/lv_image_decoder_private.h>
#include <lvgl/src/misc/cache/instance/lv_image_cache.h>
#include <lvgl/src/widgets/canvas/lv_canvas_private.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORDS 8192
struct allocation {
	uintptr_t encoded;
	size_t size;
	unsigned live;
	unsigned deferred;
	unsigned external;
};
static struct allocation allocations[RECORDS];
static size_t allocation_count;
static size_t alloc_calls;
static unsigned fail_after;
static unsigned violations;
static unsigned defer_frees;
static uintptr_t protected_start;
static size_t protected_size;
static uintptr_t destruct_target;
static unsigned detach_observed;
static size_t cache_drops;

void *__real_lv_malloc(size_t size);
void *__real_lv_malloc_zeroed(size_t size);
void *__real_lv_calloc(size_t count, size_t size);
void *__real_lv_zalloc(size_t size);
void *__real_lv_realloc(void *ptr, size_t size);
void __real_lv_free(void *ptr);
void __real_lv_obj_destruct(lv_obj_t *obj);
void __real_lv_image_cache_drop(const void *src);

/* Encoded addresses keep bookkeeping from making leaked allocations reachable
 * to a conservative leak checker. Allocation IDs remain stable after frees. */
static uintptr_t encode(const void *ptr)
{
	return (uintptr_t)ptr ^ (UINTPTR_MAX / 3);
}
static void *address(size_t index)
{
	return (void *)(allocations[index].encoded ^ (UINTPTR_MAX / 3));
}
static size_t find(uintptr_t ptr)
{
	for (size_t i = allocation_count; i > 0; i--)
		if ((uintptr_t)address(i - 1) == ptr)
			return i;
	return 0;
}
static size_t containing(uintptr_t ptr, size_t size)
{
	for (size_t i = allocation_count; i > 0; i--) {
		uintptr_t start = (uintptr_t)address(i - 1);
		if (ptr >= start && ptr - start <= allocations[i - 1].size &&
		    size <= allocations[i - 1].size - (ptr - start))
			return i;
	}
	return 0;
}
static int fail_allocation(void)
{
	alloc_calls++;
	if (fail_after && --fail_after == 0)
		return 1;
	return 0;
}
static void allocated(void *ptr, size_t size, int initialize)
{
	if (!ptr || !size)
		return;
	if (allocation_count == RECORDS)
		abort();
	allocations[allocation_count++] =
		(struct allocation){.encoded = encode(ptr), .size = size, .live = 1};
	/* Defined nonzero bytes let initialization mutants fail without an
	 * uninitialized-byte read. Zeroed allocator semantics stay unchanged. */
	if (initialize)
		memset(ptr, 0xa5, size);
}
void *__wrap_lv_malloc(size_t size)
{
	if (fail_allocation())
		return NULL;
	void *ptr = __real_lv_malloc(size);
	allocated(ptr, size, 1);
	return ptr;
}
void *__wrap_lv_malloc_zeroed(size_t size)
{
	if (fail_allocation())
		return NULL;
	void *ptr = __real_lv_malloc_zeroed(size);
	allocated(ptr, size, 0);
	return ptr;
}
void *__wrap_lv_calloc(size_t count, size_t size)
{
	if (fail_allocation() || (count && size > SIZE_MAX / count))
		return NULL;
	void *ptr = __real_lv_calloc(count, size);
	allocated(ptr, count * size, 0);
	return ptr;
}
void *__wrap_lv_zalloc(size_t size)
{
	if (fail_allocation())
		return NULL;
	void *ptr = __real_lv_zalloc(size);
	allocated(ptr, size, 0);
	return ptr;
}
void *__wrap_lv_realloc(void *ptr, size_t size)
{
	if (fail_allocation())
		return NULL;
	/* Capture integer identity before native realloc can retire the address. */
	size_t id = find((uintptr_t)ptr);
	size_t old_size = id ? allocations[id - 1].size : 0;
	void *result = __real_lv_realloc(ptr, size);
	if (result) {
		if (id)
			allocations[id - 1].live = 0;
		allocated(result, size, 0);
		if (size > old_size)
			memset((uint8_t *)result + old_size, 0xa5, size - old_size);
	}
	return result;
}
void __wrap_lv_free(void *ptr)
{
	alloc_calls++;
	if (!ptr)
		return;
	uintptr_t value = (uintptr_t)ptr;
	if (protected_size && value >= protected_start &&
	    value - protected_start < protected_size) {
		violations++;
		return;
	}
	size_t id = find(value);
	if (id) {
		if (!allocations[id - 1].live || allocations[id - 1].external) {
			violations++;
			return;
		}
		allocations[id - 1].live = 0;
		if (defer_frees) {
			allocations[id - 1].deferred = 1;
			return;
		}
	}
	__real_lv_free(ptr);
}
size_t text_test_alloc_calls(void)
{
	return alloc_calls;
}
void __wrap_lv_image_cache_drop(const void *src)
{
	cache_drops++;
	/* A missing-detach mutant must not forward the upstream address-of-
	 * pointer spelling to image decoding after the real descriptor closes. */
	if (destruct_target &&
	    (uintptr_t)src == destruct_target + offsetof(lv_canvas_t, draw_buf)) {
		violations++;
		return;
	}
	size_t id = src ? containing((uintptr_t)src, 1) : 0;
	if (id && !allocations[id - 1].live) {
		violations++;
		return;
	}
	__real_lv_image_cache_drop(src);
}
void __wrap_lv_obj_destruct(lv_obj_t *obj)
{
	int watched = (uintptr_t)obj == destruct_target;
	uintptr_t descriptor = watched ? (uintptr_t)((lv_canvas_t *)obj)->draw_buf : 0;
	__real_lv_obj_destruct(obj);
	/* Native tree deletion frees the allocation only after this returns. */
	if (watched) {
		lv_canvas_t *canvas = (lv_canvas_t *)obj;
		int dangling_source = descriptor && canvas->img.src_type == LV_IMAGE_SRC_VARIABLE &&
				      (uintptr_t)canvas->img.src == descriptor;
		detach_observed = canvas->draw_buf == NULL && !dangling_source;
		if (!detach_observed)
			violations++;
		destruct_target = 0;
	}
}

void canvas_test_begin(void)
{
	violations = 0;
	fail_after = 0;
	protected_size = 0;
	destruct_target = 0;
	detach_observed = 0;
	defer_frees = getenv("OVE_CANVAS_DEFER_FREES") != NULL;
}
void canvas_test_handlers(unsigned mode);
void canvas_test_cache(unsigned bytes);
void canvas_test_end(void)
{
	fail_after = 0;
	lv_display_t *display = lv_display_get_default();
	if (display)
		lv_obj_clean(lv_display_get_screen_active(display));
	destruct_target = 0;
	canvas_test_handlers(0);
	canvas_test_cache(0);
	for (size_t i = 0; i < allocation_count; i++) {
		if (allocations[i].deferred) {
			__real_lv_free(address(i));
			allocations[i].deferred = 0;
		}
	}
	defer_frees = 0;
	protected_size = 0;
}
void canvas_test_fail(unsigned after)
{
	fail_after = after;
}
void canvas_test_protect(const void *ptr, size_t size)
{
	protected_start = (uintptr_t)ptr;
	protected_size = size;
}
unsigned canvas_test_violations(void)
{
	return violations;
}
size_t canvas_test_external(void *ptr, size_t size)
{
	allocated(ptr, size, 0);
	allocations[allocation_count - 1].external = 1;
	return allocation_count;
}
size_t canvas_test_cache_drops(void)
{
	return cache_drops;
}
size_t canvas_test_id(const void *ptr)
{
	return containing((uintptr_t)ptr, 1);
}
unsigned canvas_test_live(size_t id)
{
	return id && id <= allocation_count && allocations[id - 1].live;
}
void canvas_test_watch_delete(void *obj)
{
	destruct_target = (uintptr_t)obj;
	detach_observed = 0;
}
unsigned canvas_test_detached(void)
{
	return detach_observed;
}

static lv_draw_buf_t *checked_buffer(void *obj)
{
	lv_draw_buf_t *buf = lv_canvas_get_draw_buf(obj);
	if (!buf)
		return NULL;
	size_t descriptor = containing((uintptr_t)buf, sizeof(*buf));
	if (!descriptor || !allocations[descriptor - 1].live) {
		violations++;
		return NULL;
	}
	size_t data = containing((uintptr_t)buf->data, buf->data_size);
	if (!data || !allocations[data - 1].live) {
		violations++;
		return NULL;
	}
	return buf;
}
int canvas_test_layout(void *obj, size_t *values)
{
	lv_draw_buf_t *buf = checked_buffer(obj);
	if (!buf)
		return 0;
	values[0] = buf->header.w;
	values[1] = buf->header.h;
	values[2] = buf->header.stride;
	values[3] = buf->data_size;
	values[4] = buf->header.flags;
	values[5] = (uintptr_t)buf->data;
	values[6] = canvas_test_id(buf);
	values[7] = canvas_test_id(buf->data);
	values[8] = lv_obj_get_class(obj)->instance_size;
	values[9] = allocations[values[6] - 1].size;
	return 1;
}
int canvas_test_read(void *obj, size_t offset, void *out, size_t size)
{
	lv_draw_buf_t *buf = checked_buffer(obj);
	if (!buf || offset > buf->data_size || size > buf->data_size - offset)
		return 0;
	memcpy(out, buf->data + offset, size);
	return 1;
}

static lv_draw_buf_handlers_t saved_handlers;
static unsigned handlers_active;
static size_t allocator_a_frees;
static size_t allocator_b_frees;
static void *allocator_a(size_t size, lv_color_format_t format)
{
	return saved_handlers.buf_malloc_cb(size, format);
}
static void free_a(void *ptr)
{
	allocator_a_frees++;
	saved_handlers.buf_free_cb(ptr);
}
static void free_b(void *ptr)
{
	allocator_b_frees++;
	saved_handlers.buf_free_cb(ptr);
}
static uint32_t stride_four(uint32_t width, lv_color_format_t format)
{
	uint32_t row = (width * lv_color_format_get_bpp(format) + 7) / 8;
	return (row + 3) & ~3U;
}
static void *unaligned(void *ptr, lv_color_format_t format)
{
	(void)format;
	return (uint8_t *)ptr + 1;
}
static uint32_t invalid_stride(uint32_t width, lv_color_format_t format)
{
	(void)width;
	(void)format;
	return 0;
}
static uint32_t excessive_stride(uint32_t width, lv_color_format_t format)
{
	(void)width;
	(void)format;
	return 65536;
}
void canvas_test_handlers(unsigned mode)
{
	lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_handlers();
	if (!mode) {
		if (handlers_active)
			*handlers = saved_handlers;
		handlers_active = 0;
		return;
	}
	if (!handlers_active) {
		saved_handlers = *handlers;
		handlers_active = 1;
		allocator_a_frees = allocator_b_frees = 0;
	}
	if (mode == 1) {
		handlers->buf_malloc_cb = allocator_a;
		handlers->buf_free_cb = free_a;
		handlers->width_to_stride_cb = stride_four;
	} else if (mode == 2) {
		handlers->buf_free_cb = free_b;
	} else if (mode == 4) {
		handlers->align_pointer_cb = unaligned;
	} else if (mode == 5) {
		handlers->width_to_stride_cb = invalid_stride;
	} else if (mode == 6) {
		handlers->width_to_stride_cb = excessive_stride;
	} else if (mode == 7) {
		handlers->buf_malloc_cb = NULL;
	} else if (mode == 8) {
		handlers->buf_free_cb = NULL;
	} else {
		handlers->buf_malloc_cb = NULL;
		handlers->buf_free_cb = NULL;
	}
}
size_t canvas_test_allocator_frees(unsigned which)
{
	return which ? allocator_b_frees : allocator_a_frees;
}
size_t canvas_test_symbol(void *obj)
{
	lv_image_set_src(obj, LV_SYMBOL_OK);
	return canvas_test_id(lv_image_get_src(obj));
}

_Alignas(32) static uint8_t display_pixels[320 * 8 * 4];
static void flush_ready(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}
void canvas_test_render_setup(void)
{
	lv_display_t *display = lv_display_get_default();
	lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(display, display_pixels, NULL, sizeof(display_pixels),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, flush_ready);
}
void canvas_test_display_delete(void)
{
	lv_display_delete(lv_display_get_default());
	lv_display_create(320, 240);
	canvas_test_render_setup();
}
void canvas_test_async_delete(void *obj)
{
	canvas_test_render_setup();
	lv_obj_delete_async(obj);
	lv_tick_inc(10);
	(void)lv_timer_handler();
}
void canvas_test_cache(unsigned bytes)
{
	lv_image_cache_resize(bytes, true);
}
int canvas_test_decode(void *obj, uint32_t *pixel)
{
	lv_draw_buf_t *source = checked_buffer(obj);
	if (!source)
		return 0;
	lv_image_decoder_dsc_t decoder;
	lv_image_decoder_args_t args = {.premultiply = true, .stride_align = true};
	if (lv_image_decoder_open(&decoder, source, &args) != LV_RESULT_OK)
		return 0;
	const lv_draw_buf_t *decoded = decoder.decoded;
	int valid = decoded && decoded->header.cf == LV_COLOR_FORMAT_ARGB8888;
	if (valid) {
		size_t id = containing((uintptr_t)decoded->data, decoded->data_size);
		valid = id && allocations[id - 1].live;
	}
	if (valid) {
		const lv_color32_t *color =
			(const lv_color32_t *)lv_draw_buf_goto_xy(decoded, 0, 0);
		*pixel = (uint32_t)color->alpha << 24 | (uint32_t)color->red << 16 |
			 (uint32_t)color->green << 8 | color->blue;
	} else {
		violations++;
	}
	lv_image_decoder_close(&decoder);
	return valid;
}
void *canvas_test_layer(void)
{
	return lv_malloc_zeroed(sizeof(lv_layer_t));
}
void canvas_test_rect(void *layer, int32_t width, int32_t height, uint32_t rgb)
{
	lv_draw_rect_dsc_t descriptor;
	lv_draw_rect_dsc_init(&descriptor);
	descriptor.bg_color = lv_color_hex(rgb);
	descriptor.bg_opa = 255;
	descriptor.border_width = 0;
	lv_area_t area = {0, 0, width - 1, height - 1};
	lv_draw_rect(layer, &descriptor, &area);
}
int canvas_test_layer_free(void *layer)
{
	if (((lv_layer_t *)layer)->draw_task_head) {
		violations++;
		return 0;
	}
	lv_free(layer);
	return 1;
}
int canvas_test_pool_allocator(void)
{
	return LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN;
}
size_t canvas_test_draw_alignment(void)
{
	return LV_DRAW_BUF_ALIGN;
}
