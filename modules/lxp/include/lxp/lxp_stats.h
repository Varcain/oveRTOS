/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Unified process/CPU snapshot for the Linux personality's synthetic /proc
 * (ps/top). The run-loop thread builds it each refresh from the live Linux slots
 * (g_lxp_proc[]) plus an ove_thread_list() of the RTOS kernel threads; the
 * /proc generator (svc-handler context) only READS it via the accessors. Holding
 * the table here (rather than reaching into the run loop) keeps the engine-agnostic
 * syscall layer free of run-loop symbols, so the host syscall tests link cleanly.
 */

#ifndef OVE_LINUX_STATS_H
#define OVE_LINUX_STATS_H

#include <stddef.h>
#include <stdint.h>

#define LXP_MAX_PENT 24    /* live Linux slots (<=NSLOT) + kernel threads */
#define LXP_MAX_KTHREAD 16 /* RTOS threads to track */
#define LXP_KPID_BASE 1000 /* kernel pids start here; Linux pids are 1..~16 */

struct lxp_thread_info; /* from <ove/thread.h> */

/* One entry shown by ps/top: a Linux process or an RTOS kernel thread. */
struct lxp_pentry {
	int pid;
	int ppid;
	char comm[16];	 /* program name; rendered "[name]" when is_kernel */
	char state;	 /* 'R' running, 'S' sleeping */
	uint64_t cpu_us; /* cumulative CPU time (µs) */
	int is_kernel;
	int live;
};

/* ---- written by the run-loop thread only ---------------------------------- */
void lxp_stats_reset(void); /* clear everything (at lxp_run start) */
void lxp_stats_begin(void); /* start a refresh: mark all entries not-live */
/* Add/update one entry (matched by pid). */
void lxp_stats_add(int pid, int ppid, const char *comm, char state, uint64_t cpu_us,
		       int is_kernel);
/* Charge a slice of a Linux process's CPU: accumulate (thread_running_us - baseline)
 * across the slot-thread recreate (fork/exec/nanosleep reset it), return the total. */
uint64_t lxp_stats_charge(int pid, uint64_t thread_running_us);
/* Read a Linux pid's accumulated CPU without charging (for parked procs). */
uint64_t lxp_proc_cpu_us(int pid);
/* Classify a kernel-thread name: idle (->1), a Linux "lnx" slot (->2), else 0. */
int lxp_stats_classify(const char *name);
/* Stable synthetic pid for a kernel thread name (allocated on first sighting). */
int lxp_kpid_for(const char *name);
void lxp_stats_set_cpu(uint64_t idle_us, uint64_t busy_us);

/* ---- read by the /proc generator (any context) ---------------------------- */
int lxp_pent_count(void);			     /* live entries */
const struct lxp_pentry *lxp_pent_at(int i); /* i-th LIVE entry */
const struct lxp_pentry *lxp_pent_find(int pid);
void lxp_cpu_totals(uint64_t *idle_us, uint64_t *busy_us);

#endif /* OVE_LINUX_STATS_H */
