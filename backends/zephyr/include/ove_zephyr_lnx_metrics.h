/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_ZEPHYR_LNX_METRICS_H
#define OVE_ZEPHYR_LNX_METRICS_H

#include <stdint.h>

/* Duration of the coordinator's irq_lock-protected personality snapshots.
 * Statistics bookkeeping itself runs after interrupts have been restored. */
struct ove_zephyr_lnx_critical_metrics {
	uint32_t sections;
	uint32_t max_cycles;
	uint64_t total_cycles;
};

/* Switch to a fresh window and return the completed window plus a coherent
 * lifetime snapshot. Available when CONFIG_OVE_LINUX_RT_SCOPE is enabled. */
void ove_zephyr_lnx_critical_metrics_take(struct ove_zephyr_lnx_critical_metrics *window,
					  struct ove_zephyr_lnx_critical_metrics *total);

#endif /* OVE_ZEPHYR_LNX_METRICS_H */
