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

/* Wall-clock cycles spent in the Linux guest's SVC C body. The syscall is the
 * ARM EABI number carried in r7; every guest trap uses the same svc #0
 * instruction, so the immediate does not identify the operation. */
struct ove_zephyr_lnx_svc_metrics {
	uint32_t calls;
	uint32_t min_cycles;
	uint32_t max_cycles;
	uint64_t total_cycles;
	uint32_t max_syscall;
};

/* Duration of the coordinator's irq_lock-protected personality snapshots.
 * Statistics bookkeeping itself runs after interrupts have been restored. */
struct ove_zephyr_lnx_critical_metrics {
	uint32_t sections;
	uint32_t max_cycles;
	uint64_t total_cycles;
};

/* Switch to fresh windows and return the completed windows plus coherent
 * lifetime snapshots. Available when CONFIG_OVE_LINUX_RT_SCOPE is enabled. */
void ove_zephyr_lnx_svc_metrics_take(struct ove_zephyr_lnx_svc_metrics *window,
				     struct ove_zephyr_lnx_svc_metrics *total);
void ove_zephyr_lnx_critical_metrics_take(struct ove_zephyr_lnx_critical_metrics *window,
					  struct ove_zephyr_lnx_critical_metrics *total);

/* Frequency of the cycle counter used by both metric families. */
uint32_t ove_zephyr_lnx_metrics_counter_hz(void);

#endif /* OVE_ZEPHYR_LNX_METRICS_H */
