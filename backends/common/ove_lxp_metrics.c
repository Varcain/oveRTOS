/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <limits.h>

#include "ove/lxp_metrics.h"

/*
 * The SVC handler is the sole writer. A task-context reader can be preempted
 * by it, but cannot run concurrently on the supported single-core targets.
 * Switching the active window before copying the old one leaves that window
 * quiescent. The lifetime seqlock protects its 64-bit total from a torn read.
 */
static volatile uint32_t g_active;
static struct ove_lxp_svc_metrics g_window[2] = {
	{.min_cycles = UINT32_MAX},
	{.min_cycles = UINT32_MAX},
};
static volatile uint32_t g_total_seq;
static struct ove_lxp_svc_metrics g_total = {
	.min_cycles = UINT32_MAX,
};

void ove_lxp_svc_metrics_snapshot(struct ove_lxp_svc_metrics *total)
{
	uint32_t before;
	uint32_t after;
	do {
		before = g_total_seq;
		__asm__ volatile("" ::: "memory");
		total->calls = g_total.calls;
		total->min_cycles = g_total.min_cycles;
		total->max_cycles = g_total.max_cycles;
		total->total_cycles = g_total.total_cycles;
		total->max_syscall = g_total.max_syscall;
		__asm__ volatile("" ::: "memory");
		after = g_total_seq;
	} while (before != after || (after & 1u) != 0u);
}

static void metrics_add(struct ove_lxp_svc_metrics *metrics, uint32_t syscall, uint32_t cycles)
{
	if (metrics->calls == 0u || cycles < metrics->min_cycles)
		metrics->min_cycles = cycles;
	if (metrics->calls == 0u || cycles > metrics->max_cycles) {
		metrics->max_cycles = cycles;
		metrics->max_syscall = syscall;
	}
	metrics->calls++;
	metrics->total_cycles += cycles;
}

void ove_lxp_svc_metrics_record(uint32_t syscall, uint32_t cycles)
{
	uint32_t active = g_active;
	metrics_add(&g_window[active], syscall, cycles);

	g_total_seq++;
	__asm__ volatile("" ::: "memory");
	metrics_add(&g_total, syscall, cycles);
	__asm__ volatile("" ::: "memory");
	g_total_seq++;
}

void ove_lxp_svc_metrics_take(struct ove_lxp_svc_metrics *window,
			      struct ove_lxp_svc_metrics *total)
{
	uint32_t old_active = g_active;
	g_active = old_active ^ 1u;
	__asm__ volatile("" ::: "memory");

	*window = g_window[old_active];
	g_window[old_active] = (struct ove_lxp_svc_metrics){
		.min_cycles = UINT32_MAX,
	};

	ove_lxp_svc_metrics_snapshot(total);
}
