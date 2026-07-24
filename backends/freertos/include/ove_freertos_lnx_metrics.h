/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_FREERTOS_LNX_METRICS_H
#define OVE_FREERTOS_LNX_METRICS_H

#include <stdint.h>

/* Wall-clock cycles spent in the Linux guest's SVC C body. The syscall is the
 * ARM EABI number carried in r7; every guest trap uses the same svc #0
 * instruction, so the immediate does not identify the operation. */
struct ove_freertos_lnx_svc_metrics {
	uint32_t calls;
	uint32_t min_cycles;
	uint32_t max_cycles;
	uint64_t total_cycles;
	uint32_t max_syscall;
};

/* Duration of the scheduler-suspended task snapshot used by
 * ove_thread_list(). Keeping this separate from SVC timing makes it possible
 * to distinguish guest syscall cost from host observability latency. */
struct ove_freertos_thread_snapshot_metrics {
	uint32_t calls;
	uint32_t max_cycles;
};

/* Switch to a fresh window, return the completed window and a coherent
 * lifetime snapshot. Available when CONFIG_OVE_LINUX_RT_SCOPE is enabled. */
void ove_freertos_lnx_svc_metrics_take(struct ove_freertos_lnx_svc_metrics *window,
				       struct ove_freertos_lnx_svc_metrics *total);

void ove_freertos_thread_snapshot_metrics_take(struct ove_freertos_thread_snapshot_metrics *window,
					       struct ove_freertos_thread_snapshot_metrics *total);

/* Frequency of the cycle counter used by the metrics above. */
uint32_t ove_freertos_lnx_svc_counter_hz(void);

#endif /* OVE_FREERTOS_LNX_METRICS_H */
