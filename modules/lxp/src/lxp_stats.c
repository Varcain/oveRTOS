/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Unified process/CPU snapshot for ps/top — see lxp_stats.h.
 */

#include <string.h>

#include "lxp/lxp_stats.h"

static struct lxp_pentry g_pent[LXP_MAX_PENT];
static int g_npent;
static uint64_t g_idle_us;
static uint64_t g_busy_us;

/* Stable name->pid registry for kernel threads (so top's deltas key consistently). */
static struct {
	char name[24];
	int pid;
} g_kreg[LXP_MAX_KTHREAD];
static int g_nkreg;

/* Per-Linux-pid accumulated CPU. The slot's "lnx" thread is aborted + recreated
 * across fork/exec/nanosleep (its RTOS runtime resets), so cumulative CPU is kept
 * as accum + a per-incarnation baseline. */
static struct {
	int pid;
	uint64_t accum_us;
	uint64_t baseline_us;
} g_pc[LXP_MAX_PENT];

/* Idle-thread names differ per engine: "idle"/"idle 00" (Zephyr), "IDLE"
 * (FreeRTOS), "Idle Task"/"CPU0 IDLE" (NuttX). Match "idle" case-insensitively. */
static int name_has_idle(const char *n)
{
	for (; n[0]; n++)
		if ((n[0] | 0x20) == 'i' && (n[1] | 0x20) == 'd' && (n[2] | 0x20) == 'l' &&
		    (n[3] | 0x20) == 'e')
			return 1;
	return 0;
}

int lxp_stats_classify(const char *name)
{
	if (!name)
		return 0;
	if (name_has_idle(name))
		return 1;
	if (name[0] == 'l' && name[1] == 'n' && name[2] == 'x') /* a Linux program slot */
		return 2;
	return 0;
}

int lxp_kpid_for(const char *name)
{
	for (int i = 0; i < g_nkreg; i++)
		if (strcmp(g_kreg[i].name, name) == 0)
			return g_kreg[i].pid;
	if (g_nkreg >= LXP_MAX_KTHREAD)
		return LXP_KPID_BASE + LXP_MAX_KTHREAD; /* overflow: shared bucket */
	int idx = g_nkreg++;
	size_t m = strlen(name);
	if (m >= sizeof(g_kreg[idx].name))
		m = sizeof(g_kreg[idx].name) - 1;
	memcpy(g_kreg[idx].name, name, m);
	g_kreg[idx].name[m] = '\0';
	g_kreg[idx].pid = LXP_KPID_BASE + idx;
	return g_kreg[idx].pid;
}

void lxp_stats_reset(void)
{
	memset(g_pent, 0, sizeof(g_pent));
	memset(g_pc, 0, sizeof(g_pc));
	memset(g_kreg, 0, sizeof(g_kreg));
	g_npent = 0;
	g_nkreg = 0;
	g_idle_us = 0;
	g_busy_us = 0;
}

void lxp_stats_begin(void)
{
	for (int i = 0; i < g_npent; i++)
		g_pent[i].live = 0;
}

void lxp_stats_add(int pid, int ppid, const char *comm, char state, uint64_t cpu_us,
		       int is_kernel)
{
	int slot = -1;
	for (int i = 0; i < g_npent; i++)
		if (g_pent[i].pid == pid) {
			slot = i;
			break;
		}
	if (slot < 0) {
		if (g_npent >= LXP_MAX_PENT)
			return;
		slot = g_npent++;
	}
	struct lxp_pentry *e = &g_pent[slot];
	e->pid = pid;
	e->ppid = ppid;
	size_t m = comm ? strlen(comm) : 0;
	if (m >= sizeof(e->comm))
		m = sizeof(e->comm) - 1;
	if (comm)
		memcpy(e->comm, comm, m);
	e->comm[m] = '\0';
	e->state = state;
	e->cpu_us = cpu_us;
	e->is_kernel = is_kernel;
	e->live = 1;
}

uint64_t lxp_stats_charge(int pid, uint64_t thread_running_us)
{
	int slot = -1, free_slot = -1;
	for (int k = 0; k < LXP_MAX_PENT; k++) {
		if (g_pc[k].pid == pid) {
			slot = k;
			break;
		}
		if (g_pc[k].pid == 0 && free_slot < 0)
			free_slot = k;
	}
	if (slot < 0 && free_slot >= 0) {
		slot = free_slot;
		g_pc[slot].pid = pid;
		g_pc[slot].accum_us = 0;
		g_pc[slot].baseline_us = thread_running_us; /* first delta is 0 */
	}
	if (slot < 0)
		return 0;
	if (thread_running_us >= g_pc[slot].baseline_us)
		g_pc[slot].accum_us += thread_running_us - g_pc[slot].baseline_us;
	g_pc[slot].baseline_us = thread_running_us; /* recreate (drop below baseline) resets */
	return g_pc[slot].accum_us;
}

uint64_t lxp_proc_cpu_us(int pid)
{
	for (int k = 0; k < LXP_MAX_PENT; k++)
		if (g_pc[k].pid == pid)
			return g_pc[k].accum_us;
	return 0;
}

void lxp_stats_set_cpu(uint64_t idle_us, uint64_t busy_us)
{
	g_idle_us = idle_us;
	g_busy_us = busy_us;
}

int lxp_pent_count(void)
{
	int c = 0;
	for (int i = 0; i < g_npent; i++)
		if (g_pent[i].live)
			c++;
	return c;
}

const struct lxp_pentry *lxp_pent_at(int i)
{
	int c = 0;
	for (int k = 0; k < g_npent; k++)
		if (g_pent[k].live && c++ == i)
			return &g_pent[k];
	return NULL;
}

const struct lxp_pentry *lxp_pent_find(int pid)
{
	for (int k = 0; k < g_npent; k++)
		if (g_pent[k].live && g_pent[k].pid == pid)
			return &g_pent[k];
	return NULL;
}

void lxp_cpu_totals(uint64_t *idle_us, uint64_t *busy_us)
{
	if (idle_us)
		*idle_us = g_idle_us;
	if (busy_us)
		*busy_us = g_busy_us;
}
