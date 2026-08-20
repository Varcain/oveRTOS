/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-facing observability facade for an embedded LXP host.
 */
#ifndef OVE_LXP_OBSERVABILITY_H
#define OVE_LXP_OBSERVABILITY_H

#include <stddef.h>
#include <stdint.h>

#include "ove/lxp_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OVE_LXP_HOST_OBSERVATION_ABI_VERSION 1u
#define OVE_LXP_DIAGNOSTICS_ABI_VERSION 1u
#define OVE_LXP_LATENCY_BUCKETS 8u
#define OVE_LXP_LATENCY_SERVICE_CAPACITY 15u
#define OVE_LXP_LATENCY_WAKE_CAPACITY 16u

/** Minimal live heartbeat used by product watchdog policy. */
typedef struct ove_lxp_run_health {
	uint32_t coordinator_iterations;
	uint32_t active;
} ove_lxp_run_health_t;

/** Exact target-ABI sizes compiled into the personality. */
typedef struct ove_lxp_size_observation {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t slots;
	uint32_t regions;
	size_t proc;
	size_t mm;
	size_t files;
	size_t fs;
	size_t sighand;
	size_t thread_group;
	size_t arena;
	size_t exec_capture;
	size_t resume_context;
	size_t deferred_request;
	size_t signal_save_stack;
	size_t vfork_guard;
	size_t debug_record;
	size_t per_slot_core;
	size_t per_region_core;
	size_t slot_table;
	size_t coordinator_static;
} ove_lxp_size_observation_t;

/** One world-validation failure copied without exposing canonical diagnostics. */
typedef struct ove_lxp_diagnostic_error {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t issue;
	int32_t slot;
	int32_t region;
	uint32_t actual;
	uint32_t expected;
} ove_lxp_diagnostic_error_t;

/** Automatic world-checkpoint health from the most recently completed run. */
typedef struct ove_lxp_diagnostics_observation {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t checks;
	uint32_t failures;
	ove_lxp_diagnostic_error_t first_error;
	ove_lxp_diagnostic_error_t last_error;
} ove_lxp_diagnostics_observation_t;

/** Bounded latency histogram: <1, <2, ... <64, and >=64 microseconds. */
typedef struct ove_lxp_latency_stat {
	uint32_t count;
	uint32_t max_ns;
	uint32_t buckets[OVE_LXP_LATENCY_BUCKETS];
} ove_lxp_latency_stat_t;

/** One copied service-class or guest-slot latency row. */
typedef struct ove_lxp_latency_observation {
	uint32_t id;
	ove_lxp_latency_stat_t stat;
} ove_lxp_latency_observation_t;

/** Normalized aggregate of native guest-task stack use. */
typedef struct ove_lxp_guest_stack_observation {
	size_t used;
	size_t size;
	uint32_t available;
} ove_lxp_guest_stack_observation_t;

/** Self-contained observation of the most recently completed host run. */
typedef struct ove_lxp_host_observation {
	uint32_t abi_version;
	uint32_t struct_size;
	ove_lxp_run_health_t run_health;
	ove_lxp_size_observation_t sizes;
	ove_lxp_diagnostics_observation_t diagnostics;
	ove_lxp_guest_stack_observation_t guest_stack;
#if defined(CONFIG_OVE_LINUX_LATENCY)
	uint32_t latency_service_count;
	uint32_t latency_wake_count;
	ove_lxp_latency_observation_t latency_services[OVE_LXP_LATENCY_SERVICE_CAPACITY];
	ove_lxp_latency_observation_t latency_wakes[OVE_LXP_LATENCY_WAKE_CAPACITY];
#endif
} ove_lxp_host_observation_t;

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
