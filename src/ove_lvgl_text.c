/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */
#include "ove/lvgl_internal.h"

#ifdef CONFIG_OVE_LVGL
#if defined(__ZEPHYR__)
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif

#ifndef OVE_LVGL_STUB
#if defined(__ZEPHYR__)
#include <src/core/lv_obj_private.h>
#include <src/core/lv_observer_private.h>
#include <src/misc/lv_event_private.h>
#else
#include <lvgl/src/core/lv_obj_private.h>
#include <lvgl/src/core/lv_observer_private.h>
#include <lvgl/src/misc/lv_event_private.h>
#endif

struct ove_lvgl_text_watch {
	lv_obj_t *obj;
	lv_event_dsc_t *event;
	void *data;
	ove_lvgl_text_release_fn release;
	unsigned refs;
};

static lv_event_cb_t unsubscribe_cb;
static uint32_t next_cookie;
static void binding_changed(lv_observer_t *observer, lv_subject_t *subject);

void *ove_lvgl_text_alloc(size_t size)
{
	return lv_is_initialized() ? lv_malloc(size) : NULL;
}

void *ove_lvgl_text_realloc(void *ptr, size_t size)
{
	return lv_is_initialized() ? lv_realloc(ptr, size) : NULL;
}

void ove_lvgl_text_free(void *ptr)
{
	if (ptr)
		lv_free(ptr);
}

static void watch_unref(ove_lvgl_text_watch_t *watch)
{
	if (--watch->refs == 0)
		lv_free(watch);
}

static void watch_cancel(ove_lvgl_text_watch_t *watch)
{
	watch->obj = NULL;
	if (watch->release) {
		watch->release(watch->data);
		watch->release = NULL;
	}
}

static void watch_deleted(lv_event_t *event)
{
	ove_lvgl_text_watch_t *watch = lv_event_get_user_data(event);
	watch_cancel(watch);
	watch_unref(watch);
}

static int usable(lv_obj_t *obj)
{
	return obj && lv_obj_is_valid(obj) && !obj->is_deleting;
}

ove_lvgl_text_watch_t *ove_lvgl_text_watch(void *obj, void *data, ove_lvgl_text_release_fn release)
{
	if (!usable(obj))
		return NULL;
	ove_lvgl_text_watch_t *watch = lv_malloc(sizeof(*watch));
	if (!watch)
		return NULL;
	*watch = (ove_lvgl_text_watch_t){.obj = obj, .data = data, .release = release, .refs = 2};
	watch->event = lv_obj_add_event_cb(obj, watch_deleted, LV_EVENT_DELETE, watch);
	if (!watch->event) {
		lv_free(watch);
		return NULL;
	}
	return watch;
}

int ove_lvgl_text_watch_valid(ove_lvgl_text_watch_t *watch)
{
	return watch->obj != NULL;
}

void ove_lvgl_text_watch_end(ove_lvgl_text_watch_t *watch)
{
	if (watch->obj) {
		lv_obj_remove_event_dsc(watch->obj, watch->event);
		watch_cancel(watch);
		watch_unref(watch);
	}
	watch_unref(watch);
}

/* Removed descriptors remain visible until an active event walk finishes. */
static int live_event(lv_event_dsc_t *event)
{
	return !(event->filter & LV_EVENT_MARKED_DELETING);
}

static lv_observer_t *find_binding(lv_obj_t *obj)
{
	if (!unsubscribe_cb || !usable(obj))
		return NULL;
	for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
		lv_event_dsc_t *event = lv_obj_get_event_dsc(obj, i);
		if (!live_event(event) || lv_event_dsc_get_cb(event) != unsubscribe_cb)
			continue;
		lv_observer_t *observer = lv_event_dsc_get_user_data(event);
		if (observer && observer->cb == binding_changed)
			return observer;
	}
	return NULL;
}

static void unbind(lv_obj_t *obj)
{
	lv_observer_t *observer;
	while ((observer = find_binding(obj)))
		lv_observer_remove(observer);
}

static void release_storage(lv_obj_t *obj)
{
	for (uint32_t i = 0; i < lv_obj_get_event_count(obj);) {
		lv_event_dsc_t *event = lv_obj_get_event_dsc(obj, i);
		if (live_event(event) && lv_event_dsc_get_cb(event) == watch_deleted) {
			ove_lvgl_text_watch_t *watch = lv_event_dsc_get_user_data(event);
			if (watch->release) {
				lv_obj_remove_event_dsc(obj, event);
				watch_cancel(watch);
				watch_unref(watch);
				i = 0;
				continue;
			}
		}
		i++;
	}
}

void ove_lvgl_text_set(void *obj, const char *text)
{
	unbind(obj);
	release_storage(obj);
	lv_label_set_text(obj, text);
}

int ove_lvgl_text_static(void *obj, const char *text, void *data, ove_lvgl_text_release_fn release)
{
	ove_lvgl_text_watch_t *watch = ove_lvgl_text_watch(obj, data, release);
	if (!watch)
		return OVE_ERR_NO_MEMORY;
	unbind(obj);
	/* The newly attached owner must survive the previous owner's release. */
	watch->release = NULL;
	release_storage(obj);
	watch->release = release;
	lv_label_set_text_static(obj, text);
	int valid = ove_lvgl_text_watch_valid(watch);
	watch_unref(watch);
	return valid ? OVE_OK : OVE_ERR_BAD_HANDLE;
}

int ove_lvgl_text_current(void *obj, uint32_t cookie, int32_t value)
{
	lv_observer_t *observer = find_binding(obj);
	if (!observer)
		return OVE_ERR_NOT_FOUND;
	ove_lvgl_text_binding_t *context = observer->user_data;
	if (context->cookie != cookie)
		return OVE_ERR_NOT_FOUND;
	return lv_subject_get_int(observer->subject) == value ? OVE_OK : OVE_ERR_BUSY;
}

int ove_lvgl_text_publish(void *obj, uint32_t cookie, int32_t value, const char *text)
{
	int result = ove_lvgl_text_current(obj, cookie, value);
	if (result)
		return result;
	release_storage(obj);
	lv_label_set_text(obj, text);
	return OVE_OK;
}

static void binding_changed(lv_observer_t *observer, lv_subject_t *subject)
{
	lv_obj_t *obj = observer->target;
	ove_lvgl_text_binding_t *context = observer->user_data;
	uint32_t cookie = context->cookie;
	ove_lvgl_text_render_fn render = context->render;
	int32_t value = lv_subject_get_int(subject);
	/* User formatting may remove this observer and free its context. */
	int status = render(obj, cookie, value, context);
	observer = find_binding(obj);
	if (observer) {
		context = observer->user_data;
		if (context->cookie == cookie)
			context->status = status;
	}
}

int ove_lvgl_text_bind(void *obj, void *subject, ove_lvgl_text_binding_t *context)
{
	if (!usable(obj) || next_cookie == UINT32_MAX) {
		lv_free(context);
		return OVE_ERR_BAD_HANDLE;
	}
	unbind(obj);
	context->cookie = ++next_cookie;
	uint32_t cookie = context->cookie;
	context->status = OVE_OK;
	/* NULL prevents LVGL's eager callback until ownership is installed. */
	lv_observer_t *observer = lv_subject_add_observer_obj(subject, NULL, obj, context);
	if (!observer) {
		lv_free(context);
		return OVE_ERR_NO_MEMORY;
	}
	for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
		lv_event_dsc_t *event = lv_obj_get_event_dsc(obj, i);
		if (live_event(event) && lv_event_dsc_get_user_data(event) == observer)
			unsubscribe_cb = lv_event_dsc_get_cb(event);
	}
	observer->auto_free_user_data = 1;
	observer->cb = binding_changed;
	binding_changed(observer, subject);
	observer = find_binding(obj);
	if (!observer)
		return OVE_ERR_NOT_FOUND;
	context = observer->user_data;
	if (context->cookie != cookie)
		return OVE_ERR_NOT_FOUND;
	int status = context->status;
	if (status)
		lv_observer_remove(observer);
	return status;
}

int ove_lvgl_text_status(void *obj)
{
	lv_observer_t *observer = find_binding(obj);
	return observer ? ((ove_lvgl_text_binding_t *)observer->user_data)->status
			: OVE_ERR_NOT_FOUND;
}
#else
#include <stdlib.h>
void *ove_lvgl_text_alloc(size_t size)
{
	return malloc(size);
}
void *ove_lvgl_text_realloc(void *ptr, size_t size)
{
	return realloc(ptr, size);
}
void ove_lvgl_text_free(void *ptr)
{
	free(ptr);
}
ove_lvgl_text_watch_t *ove_lvgl_text_watch(void *obj, void *data, ove_lvgl_text_release_fn release)
{
	(void)obj;
	(void)data;
	(void)release;
	return NULL;
}
int ove_lvgl_text_watch_valid(ove_lvgl_text_watch_t *watch)
{
	(void)watch;
	return 0;
}
void ove_lvgl_text_watch_end(ove_lvgl_text_watch_t *watch)
{
	(void)watch;
}
void ove_lvgl_text_set(void *obj, const char *text)
{
	lv_label_set_text(obj, text);
}
int ove_lvgl_text_static(void *obj, const char *text, void *data, ove_lvgl_text_release_fn release)
{
	(void)obj;
	(void)text;
	(void)data;
	(void)release;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_lvgl_text_bind(void *obj, void *subject, ove_lvgl_text_binding_t *context)
{
	(void)obj;
	(void)subject;
	free(context);
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_lvgl_text_current(void *obj, uint32_t cookie, int32_t value)
{
	(void)obj;
	(void)cookie;
	(void)value;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_lvgl_text_status(void *obj)
{
	(void)obj;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_lvgl_text_publish(void *obj, uint32_t cookie, int32_t value, const char *text)
{
	(void)text;
	return ove_lvgl_text_current(obj, cookie, value);
}
#endif /* OVE_LVGL_STUB */
#endif /* CONFIG_OVE_LVGL */
