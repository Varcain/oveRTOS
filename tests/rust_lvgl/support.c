/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */
#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>

void ove_lvgl_assert_failed(const char *file, int line)
{
	fprintf(stderr, "LVGL assert: %s:%d\n", file, line);
	abort();
}

void *text_test_screen(void)
{
	if (!lv_is_initialized()) {
		lv_init();
		lv_display_create(320, 240);
	}
	lv_obj_t *screen = lv_screen_active();
	lv_obj_clean(screen);
	return screen;
}

void ove_lvgl_lock(void)
{
	lv_lock();
}
void ove_lvgl_unlock(void)
{
	lv_unlock();
}

const char *text_test_label(void *obj)
{
	return lv_label_get_text(obj);
}
size_t text_test_used(void)
{
	lv_mem_monitor_t monitor;
	lv_mem_monitor(&monitor);
	return monitor.total_size - monitor.free_size;
}

void *text_test_subject_observer(void *obj, void *subject)
{
	return lv_subject_add_observer_obj(subject, NULL, obj, NULL);
}
void text_test_remove_observer(void *observer)
{
	lv_observer_remove(observer);
}
void text_test_send(void *obj)
{
	lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
}
