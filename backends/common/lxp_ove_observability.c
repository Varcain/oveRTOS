/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Narrow oveRTOS facade over LXP-owned observability state.
 */

#include "ove/lxp_observability.h"
#include "lxp_ove_host_internal.h"

#include <string.h>

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
	if (!host || !out) {
		if (out)
			memset(out, 0, sizeof(*out));
		return OVE_ERR_INVALID_PARAM;
	}
	const ove_lxp_host_impl_t *impl = ove_lxp_host_private_const(host);
	int rc = lxp_host_observe(&impl->core, out);
	if (rc == LXP_OK)
		return OVE_OK;
	return rc == LXP_ERR_BUSY ? OVE_ERR_BUSY : OVE_ERR_INVALID_PARAM;
}

void ove_lxp_latency_record(ove_lxp_latency_stat_t *stat, uint64_t ns)
{
	lxp_lat_record(stat, ns);
}

const char *ove_lxp_observation_service_name(const ove_lxp_host_observation_t *observation,
					     unsigned row)
{
	return lxp_host_observation_service_name(observation, row);
}

const char *ove_lxp_observation_issue_name(unsigned issue)
{
	return lxp_diag_issue_name(issue);
}
