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

/* Duration of the scheduler-suspended task snapshot used by
 * ove_thread_list(). Keeping this separate from SVC timing makes it possible
 * to distinguish guest syscall cost from host observability latency. */
struct ove_freertos_thread_snapshot_metrics {
	uint32_t calls;
	uint32_t max_cycles;
};

void ove_freertos_thread_snapshot_metrics_take(struct ove_freertos_thread_snapshot_metrics *window,
					       struct ove_freertos_thread_snapshot_metrics *total);

#endif /* OVE_FREERTOS_LNX_METRICS_H */
