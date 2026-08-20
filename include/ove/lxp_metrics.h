/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LXP_METRICS_H
#define OVE_LXP_METRICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wall-clock cycles spent in the Linux guest's SVC C body. The syscall is the
 * ARM EABI number carried in r7; every guest trap uses the same svc #0
 * instruction, so the immediate does not identify the operation. */
struct ove_lxp_svc_metrics {
	uint32_t calls;
	uint32_t min_cycles;
	uint32_t max_cycles;
	uint64_t total_cycles;
	uint32_t max_syscall;
};

/* Switch to a fresh window, return the completed window and a coherent
 * lifetime snapshot translated from canonical LXP ownership. */
void ove_lxp_svc_metrics_take(struct ove_lxp_svc_metrics *window,
			      struct ove_lxp_svc_metrics *total);

/* Return the lifetime counters without switching or resetting the report
 * window. Safe for procfs readers in task/SVC context. */
void ove_lxp_svc_metrics_snapshot(struct ove_lxp_svc_metrics *total);

/* Canonical LXP diagnostic name for an ARM EABI syscall number. */
const char *ove_lxp_syscall_name(uint32_t syscall_nr);

/* Engine implementation supplies the frequency of its SVC cycle counter. */
uint32_t ove_lxp_metrics_counter_hz(void);

struct ove_lxp_critical_metrics {
	uint32_t sections;
	uint32_t max_cycles;
	uint64_t total_cycles;
};

/* Optional LXP coordinator critical-section timing. Returns OVE_OK when the
 * selected port provides the metric, OVE_ERR_NOT_SUPPORTED otherwise. */
int ove_lxp_critical_metrics_take(struct ove_lxp_critical_metrics *window,
				  struct ove_lxp_critical_metrics *total);

struct ove_lxp_thread_snapshot_metrics {
	uint32_t calls;
	uint32_t max_cycles;
};

/* Optional duration of the host thread snapshot used by LXP observability. */
int ove_lxp_thread_snapshot_metrics_take(struct ove_lxp_thread_snapshot_metrics *window,
					 struct ove_lxp_thread_snapshot_metrics *total);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_METRICS_H */
