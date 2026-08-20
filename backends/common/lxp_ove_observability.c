/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Narrow oveRTOS facade over LXP-owned observability state.
 */

#include "ove/lxp_observability.h"
#include "lxp_ove_host_internal.h"
#include "lxp/lxp_observe.h"

#include <string.h>

_Static_assert(LXP_HOST_OBSERVATION_ABI_VERSION == 1u,
	       "update the OVE host-observation translator for the new LXP ABI");
_Static_assert(LXP_DIAG_ABI_VERSION == 1u,
	       "update the OVE diagnostic translator for the new LXP ABI");
_Static_assert(LXP_LAT_BUCKETS == OVE_LXP_LATENCY_BUCKETS,
	       "OVE and LXP latency histogram contracts differ");
_Static_assert(LXP_LAT_CLASSES - 1 == OVE_LXP_LATENCY_SERVICE_CAPACITY,
	       "OVE service observation capacity is stale");
_Static_assert(LXP_NSLOT <= OVE_LXP_LATENCY_WAKE_CAPACITY,
	       "OVE wake observation capacity is too small");

#if defined(CONFIG_OVE_LINUX_LATENCY)
_Static_assert(LXP_ENABLE_LATENCY == 1, "OVE latency enabled without the LXP recorder");
#else
_Static_assert(LXP_ENABLE_LATENCY == 0, "LXP latency enabled without the OVE contract");
#endif

static int observation_contract_is_current(const lxp_host_observation_t *in)
{
	return in->abi_version == LXP_HOST_OBSERVATION_ABI_VERSION &&
	       in->struct_size == sizeof(*in) && in->sizes.abi_version == LXP_DIAG_ABI_VERSION &&
	       in->sizes.struct_size == sizeof(in->sizes) &&
	       in->diagnostics.abi_version == LXP_DIAG_ABI_VERSION &&
	       in->diagnostics.struct_size == sizeof(in->diagnostics) &&
	       in->diagnostics.first_error.abi_version == LXP_DIAG_ABI_VERSION &&
	       in->diagnostics.first_error.struct_size == sizeof(in->diagnostics.first_error) &&
	       in->diagnostics.last_error.abi_version == LXP_DIAG_ABI_VERSION &&
	       in->diagnostics.last_error.struct_size == sizeof(in->diagnostics.last_error);
}

static void copy_sizes(ove_lxp_size_observation_t *out, const lxp_diag_size_report_t *in)
{
	out->abi_version = OVE_LXP_DIAGNOSTICS_ABI_VERSION;
	out->struct_size = sizeof(*out);
	out->slots = in->slots;
	out->regions = in->regions;
	out->proc = in->proc;
	out->mm = in->mm;
	out->files = in->files;
	out->fs = in->fs;
	out->sighand = in->sighand;
	out->thread_group = in->thread_group;
	out->arena = in->arena;
	out->exec_capture = in->exec_capture;
	out->resume_context = in->resume_context;
	out->deferred_request = in->deferred_request;
	out->signal_save_stack = in->signal_save_stack;
	out->vfork_guard = in->vfork_guard;
	out->debug_record = in->debug_record;
	out->per_slot_core = in->per_slot_core;
	out->per_region_core = in->per_region_core;
	out->slot_table = in->slot_table;
	out->coordinator_static = in->coordinator_static;
}

static void copy_error(ove_lxp_diagnostic_error_t *out, const lxp_diag_error_t *in)
{
	out->abi_version = OVE_LXP_DIAGNOSTICS_ABI_VERSION;
	out->struct_size = sizeof(*out);
	out->issue = in->issue;
	out->slot = in->slot;
	out->region = in->region;
	out->actual = in->actual;
	out->expected = in->expected;
}

static void copy_diagnostics(ove_lxp_diagnostics_observation_t *out, const lxp_diag_health_t *in)
{
	out->abi_version = OVE_LXP_DIAGNOSTICS_ABI_VERSION;
	out->struct_size = sizeof(*out);
	out->checks = in->checks;
	out->failures = in->failures;
	copy_error(&out->first_error, &in->first_error);
	copy_error(&out->last_error, &in->last_error);
}

#if defined(CONFIG_OVE_LINUX_LATENCY)
static void copy_latency_stat(ove_lxp_latency_stat_t *out, const lxp_lat_stat_t *in)
{
	out->count = in->count;
	out->max_ns = in->max_ns;
	for (unsigned bucket = 0u; bucket < OVE_LXP_LATENCY_BUCKETS; bucket++)
		out->buckets[bucket] = in->buckets[bucket];
}

static void copy_latency_row(ove_lxp_latency_observation_t *out,
			     const lxp_latency_observation_t *in)
{
	out->id = in->id;
	copy_latency_stat(&out->stat, &in->stat);
}
#endif

void ove_lxp_run_health_snapshot(ove_lxp_run_health_t *out)
{
	if (!out)
		return;
	lxp_run_health_t health = {0};
	lxp_run_health(&health);
	out->coordinator_iterations = health.coord_iters;
	out->active = health.active != 0;
}

int ove_lxp_host_observe(const ove_lxp_host_t *host, ove_lxp_host_observation_t *out)
{
	int rc;

	if (!out)
		return OVE_ERR_INVALID_PARAM;
	if (!host)
		goto invalid;
	const ove_lxp_host_impl_t *impl = ove_lxp_host_private_const(host);
	lxp_host_observation_t observation;
	rc = lxp_host_observe(&impl->core, &observation);
	if (rc != LXP_OK) {
		rc = rc == LXP_ERR_BUSY ? OVE_ERR_BUSY : OVE_ERR_INVALID_PARAM;
		goto clear;
	}
	if (!observation_contract_is_current(&observation))
		goto invalid;

#if defined(CONFIG_OVE_LINUX_LATENCY)
	if (observation.latency_service_count > OVE_LXP_LATENCY_SERVICE_CAPACITY ||
	    observation.latency_wake_count > OVE_LXP_LATENCY_WAKE_CAPACITY)
		goto invalid;
#endif

	memset(out, 0, sizeof(*out));
	out->abi_version = OVE_LXP_HOST_OBSERVATION_ABI_VERSION;
	out->struct_size = sizeof(*out);
	out->run_health.coordinator_iterations = observation.run_health.coord_iters;
	out->run_health.active = observation.run_health.active != 0;
	copy_sizes(&out->sizes, &observation.sizes);
	copy_diagnostics(&out->diagnostics, &observation.diagnostics);
	out->guest_stack.used = observation.guest_stack.used;
	out->guest_stack.size = observation.guest_stack.size;
	out->guest_stack.available = observation.guest_stack.available != 0;

#if defined(CONFIG_OVE_LINUX_LATENCY)
	out->latency_service_count = observation.latency_service_count;
	out->latency_wake_count = observation.latency_wake_count;
	for (unsigned row = 0u; row < observation.latency_service_count; row++)
		copy_latency_row(&out->latency_services[row], &observation.latency_services[row]);
	for (unsigned row = 0u; row < observation.latency_wake_count; row++)
		copy_latency_row(&out->latency_wakes[row], &observation.latency_wakes[row]);
#endif
	return OVE_OK;

invalid:
	rc = OVE_ERR_INVALID_PARAM;
clear:
	memset(out, 0, sizeof(*out));
	return rc;
}

void ove_lxp_latency_record(ove_lxp_latency_stat_t *stat, uint64_t ns)
{
#if defined(CONFIG_OVE_LINUX_LATENCY)
	if (!stat)
		return;
	lxp_lat_stat_t canonical = {
		.count = stat->count,
		.max_ns = stat->max_ns,
	};
	for (unsigned bucket = 0u; bucket < OVE_LXP_LATENCY_BUCKETS; bucket++)
		canonical.buckets[bucket] = stat->buckets[bucket];
	lxp_lat_record(&canonical, ns);
	copy_latency_stat(stat, &canonical);
#else
	(void)stat;
	(void)ns;
#endif
}

const char *ove_lxp_observation_service_name(const ove_lxp_host_observation_t *observation,
					     unsigned row)
{
#if defined(CONFIG_OVE_LINUX_LATENCY)
	if (!observation || observation->abi_version != OVE_LXP_HOST_OBSERVATION_ABI_VERSION ||
	    observation->struct_size != sizeof(*observation) ||
	    row >= observation->latency_service_count)
		return "?";
	return lxp_lat_class_name((int)observation->latency_services[row].id);
#else
	(void)observation;
	(void)row;
	return "?";
#endif
}

const char *ove_lxp_observation_issue_name(unsigned issue)
{
	return lxp_diag_issue_name(issue);
}
