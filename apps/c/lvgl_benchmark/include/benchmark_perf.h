/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared benchmark performance helper.
 *
 * Provides access to LVGL's perf monitor subject and metric extraction
 * without requiring private LVGL headers in application code.
 */

#ifndef BENCHMARK_PERF_H
#define BENCHMARK_PERF_H

#include "ove/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t fps;
	uint32_t cpu;
	uint32_t render_avg_time;
	uint32_t flush_avg_time;
} benchmark_perf_metrics_t;

/**
 * @brief Get the perf monitor subject from the default display.
 * @return Pointer to lv_subject_t, or NULL if perf monitor unavailable.
 */
lv_subject_t *benchmark_get_perf_subject(void);

/**
 * @brief Extract performance metrics from a perf observer callback.
 * @param info  The pointer obtained via lv_subject_get_pointer(subject).
 * @param out   Output metrics struct (zeroed if perf monitor unavailable).
 */
void benchmark_extract_perf_metrics(const void *info,
				    benchmark_perf_metrics_t *out);

/*
 * Animation helpers — thin wrappers around lv_anim_t for languages where
 * the type is opaque (Zig cImport cannot resolve its layout).
 */
void benchmark_anim_color(lv_obj_t *obj, lv_anim_exec_xcb_t cb);
void benchmark_anim_shake(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
			  int32_t y_max, uint32_t t1, uint32_t t2);
void benchmark_anim_scroll(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
			   int32_t y_max, uint32_t t);
void benchmark_anim_arc(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
			uint32_t t1, uint32_t t2);

/** General animation: custom value range + durations with playback. */
void benchmark_anim_generic(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
			    int32_t start, int32_t end,
			    uint32_t t1, uint32_t t2);

/** Slideshow: scroll obj to y_max then call ready_cb on completion. */
void benchmark_anim_slideshow(lv_obj_t *obj, lv_anim_exec_xcb_t scroll_cb,
			      int32_t y_max, uint32_t speed,
			      lv_anim_completed_cb_t ready_cb);

/**
 * @brief Draw task callback for summary table header/separator styling.
 *
 * Row 0 (header): dark blue-grey background, white text.
 * Row 1 (averages): dark bottom border, dark text.
 *
 * Register via lv_obj_add_event_cb(table, benchmark_table_draw_task_cb,
 *                                  LV_EVENT_DRAW_TASK_ADDED, NULL).
 */
void benchmark_table_draw_task_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif

#endif /* BENCHMARK_PERF_H */
