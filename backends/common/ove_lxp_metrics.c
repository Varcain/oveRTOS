/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "lxp/lxp_rt_metrics.h"
#include "ove/lxp_metrics.h"
#include "ove/types.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_RTOS_ZEPHYR)
#include "lxp/ports/zephyr.h"
#endif

static void svc_metrics_copy(struct ove_lxp_svc_metrics *out,
			     const lxp_rt_svc_metrics_t *source)
{
	out->calls = source->calls;
	out->min_cycles = source->min_cycles;
	out->max_cycles = source->max_cycles;
	out->total_cycles = source->total_cycles;
	out->max_syscall = source->max_syscall;
}

void ove_lxp_svc_metrics_snapshot(struct ove_lxp_svc_metrics *total)
{
	lxp_rt_svc_metrics_t source;
	if (!total)
		return;
	lxp_rt_svc_metrics_snapshot(&source);
	svc_metrics_copy(total, &source);
}

void ove_lxp_svc_metrics_take(struct ove_lxp_svc_metrics *window,
			      struct ove_lxp_svc_metrics *total)
{
	lxp_rt_svc_metrics_t source_window;
	lxp_rt_svc_metrics_t source_total;
	if (!window || !total)
		return;
	lxp_rt_svc_metrics_take(&source_window, &source_total);
	svc_metrics_copy(window, &source_window);
	svc_metrics_copy(total, &source_total);
}

const char *ove_lxp_syscall_name(uint32_t syscall_nr)
{
	return lxp_rt_syscall_name(syscall_nr);
}

int ove_lxp_critical_metrics_take(struct ove_lxp_critical_metrics *window,
				  struct ove_lxp_critical_metrics *total)
{
	if (!window || !total)
		return OVE_ERR_INVALID_PARAM;
#if defined(CONFIG_OVE_RTOS_ZEPHYR)
	lxp_zephyr_critical_metrics_t source_window;
	lxp_zephyr_critical_metrics_t source_total;
	lxp_zephyr_critical_metrics_take(&source_window, &source_total);
	*window = (struct ove_lxp_critical_metrics){
		.sections = source_window.sections,
		.max_cycles = source_window.max_cycles,
		.total_cycles = source_window.total_cycles,
	};
	*total = (struct ove_lxp_critical_metrics){
		.sections = source_total.sections,
		.max_cycles = source_total.max_cycles,
		.total_cycles = source_total.total_cycles,
	};
	return OVE_OK;
#else
	*window = (struct ove_lxp_critical_metrics){0};
	*total = (struct ove_lxp_critical_metrics){0};
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

#if !defined(CONFIG_OVE_RTOS_FREERTOS)
int ove_lxp_thread_snapshot_metrics_take(struct ove_lxp_thread_snapshot_metrics *window,
					 struct ove_lxp_thread_snapshot_metrics *total)
{
	if (!window || !total)
		return OVE_ERR_INVALID_PARAM;
	*window = (struct ove_lxp_thread_snapshot_metrics){0};
	*total = (struct ove_lxp_thread_snapshot_metrics){0};
	return OVE_ERR_NOT_SUPPORTED;
}
#endif
