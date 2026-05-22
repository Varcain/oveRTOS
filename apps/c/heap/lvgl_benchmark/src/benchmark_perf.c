/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark_perf.h"

#if LV_USE_PERF_MONITOR

#include "src/display/lv_display_private.h"
#include "src/debugging/sysmon/lv_sysmon_private.h"

lv_subject_t *benchmark_get_perf_subject(void)
{
	lv_display_t *disp = lv_display_get_default();
	if (!disp)
		return NULL;
	return &disp->perf_sysmon_backend.subject;
}

void benchmark_extract_perf_metrics(const void *info, benchmark_perf_metrics_t *out)
{
	const lv_sysmon_perf_info_t *p = info;
	out->fps = p->calculated.fps;
	out->cpu = p->calculated.cpu;
	out->render_avg_time = p->calculated.render_avg_time;
	out->flush_avg_time = p->calculated.flush_avg_time;
}

#else /* !LV_USE_PERF_MONITOR — stubs so non-C languages link cleanly */

lv_subject_t *benchmark_get_perf_subject(void)
{
	return NULL;
}

void benchmark_extract_perf_metrics(const void *info, benchmark_perf_metrics_t *out)
{
	(void)info;
	out->fps = 0;
	out->cpu = 0;
	out->render_avg_time = 0;
	out->flush_avg_time = 0;
}

#endif /* LV_USE_PERF_MONITOR */

/* Animation helpers — thin C wrappers for languages where lv_anim_t is opaque */

void benchmark_anim_color(lv_obj_t *obj, lv_anim_exec_xcb_t cb)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, 0, 100);
	lv_anim_set_duration(&a, 100);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

void benchmark_anim_shake(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t y_max, uint32_t t1,
			  uint32_t t2)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, 0, y_max);
	lv_anim_set_duration(&a, t1);
	lv_anim_set_playback_duration(&a, t2);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

void benchmark_anim_scroll(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t y_max, uint32_t t)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, 0, y_max);
	lv_anim_set_duration(&a, t);
	lv_anim_set_playback_duration(&a, t);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

void benchmark_anim_arc(lv_obj_t *obj, lv_anim_exec_xcb_t cb, uint32_t t1, uint32_t t2)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, 0, 100);
	lv_anim_set_duration(&a, t1);
	lv_anim_set_playback_duration(&a, t2);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

void benchmark_anim_generic(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t start, int32_t end,
			    uint32_t t1, uint32_t t2)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, start, end);
	lv_anim_set_duration(&a, t1);
	lv_anim_set_playback_duration(&a, t2);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
}

void benchmark_anim_slideshow(lv_obj_t *obj, lv_anim_exec_xcb_t scroll_cb, int32_t y_max,
			      uint32_t speed, lv_anim_completed_cb_t ready_cb)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, scroll_cb);
	lv_anim_set_values(&a, 0, y_max);
	lv_anim_set_duration(&a, speed);
	lv_anim_set_playback_duration(&a, speed);
	lv_anim_set_completed_cb(&a, ready_cb);
	lv_anim_start(&a);
}

/* Draw task callback for summary table — shared so Zig can use it
 * (lv_draw_buf_t is opaque in Zig due to lv_image_header_t bitfields). */

void benchmark_table_draw_task_cb(lv_event_t *e)
{
	lv_draw_task_t *t = lv_event_get_draw_task(e);
	lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
	if (base->part != LV_PART_ITEMS)
		return;

	int32_t row = base->id1;
	if (row == 0) {
		lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(t);
		if (fill)
			fill->color = lv_palette_darken(LV_PALETTE_BLUE_GREY, 4);
		lv_draw_label_dsc_t *lbl = lv_draw_task_get_label_dsc(t);
		if (lbl)
			lbl->color = lv_color_white();
	} else if (row == 1) {
		lv_draw_border_dsc_t *border = lv_draw_task_get_border_dsc(t);
		if (border) {
			border->color = lv_palette_darken(LV_PALETTE_BLUE_GREY, 4);
			border->width = 2;
			border->side = LV_BORDER_SIDE_BOTTOM;
		}
		lv_draw_label_dsc_t *lbl = lv_draw_task_get_label_dsc(t);
		if (lbl)
			lbl->color = lv_palette_darken(LV_PALETTE_BLUE_GREY, 4);
	}
}
