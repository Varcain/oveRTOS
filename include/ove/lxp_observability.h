/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-facing observability facade for an embedded LXP host.
 */
#ifndef OVE_LXP_OBSERVABILITY_H
#define OVE_LXP_OBSERVABILITY_H

#include <stdint.h>

#include "lxp/lxp_observe.h"
#include "ove/lxp_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Host-neutral aliases: consumers need no diagnostic registry or RTOS-port header. */
typedef lxp_host_observation_t ove_lxp_host_observation_t;
typedef lxp_diag_size_report_t ove_lxp_size_observation_t;
typedef lxp_diag_health_t ove_lxp_diagnostics_observation_t;
typedef lxp_lat_stat_t ove_lxp_latency_stat_t;
typedef lxp_latency_observation_t ove_lxp_latency_observation_t;

#define OVE_LXP_LATENCY_BUCKETS LXP_LAT_BUCKETS

/** Minimal live heartbeat used by product watchdog policy. */
typedef struct ove_lxp_run_health {
	uint32_t coordinator_iterations;
	uint32_t active;
} ove_lxp_run_health_t;

/** Read the single-instance coordinator heartbeat. Valid before host init. */
void ove_lxp_run_health_snapshot(ove_lxp_run_health_t *out);

/** Copy one complete, quiescent observation after ove_lxp_host_run() returns. */
int ove_lxp_host_observe(const ove_lxp_host_t *host, ove_lxp_host_observation_t *out);

/** Record a host-side latency sample with LXP's canonical bounded histogram. */
void ove_lxp_latency_record(ove_lxp_latency_stat_t *stat, uint64_t ns);

/** Stable service name for one copied latency row. */
const char *ove_lxp_observation_service_name(const ove_lxp_host_observation_t *observation,
					     unsigned row);

/** Stable diagnostic issue name for product reporting. */
const char *ove_lxp_observation_issue_name(unsigned issue);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_OBSERVABILITY_H */
