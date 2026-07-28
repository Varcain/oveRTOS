/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "lxp_ove_thread_adapter.h"

static lxp_thread_state_t thread_state_to_lxp(ove_thread_state_t state)
{
	switch (state) {
	case OVE_THREAD_STATE_RUNNING:
		return LXP_THREAD_STATE_RUNNING;
	case OVE_THREAD_STATE_READY:
		return LXP_THREAD_STATE_READY;
	case OVE_THREAD_STATE_BLOCKED:
		return LXP_THREAD_STATE_BLOCKED;
	case OVE_THREAD_STATE_SUSPENDED:
		return LXP_THREAD_STATE_SUSPENDED;
	case OVE_THREAD_STATE_TERMINATED:
		return LXP_THREAD_STATE_TERMINATED;
	case OVE_THREAD_STATE_UNKNOWN:
	default:
		return LXP_THREAD_STATE_UNKNOWN;
	}
}

static int result_to_lxp(int result)
{
	switch (result) {
	case OVE_OK:
		return LXP_OK;
	case OVE_ERR_QUEUE_FULL:
		return LXP_ERR_QUEUE_FULL;
	case OVE_ERR_NOT_SUPPORTED:
	default:
		return LXP_ERR_NOT_SUPPORTED;
	}
}

void lxp_ove_thread_info_copy(struct lxp_thread_info *out, const struct ove_thread_info *host,
			      lxp_ove_slot_lookup_t slot_lookup)
{
	out->name = host->name;
	out->identity = host->identity;
	out->lxp_slot = slot_lookup ? slot_lookup(host->identity) : LXP_THREAD_SLOT_NONE;
	out->state = thread_state_to_lxp(host->state);
	out->priority = host->priority;
	out->stack_used = host->stack_used;
	out->stack_size = host->stack_size;
	out->cpu_percent_x100 = host->cpu_percent_x100;
	out->state_times.running_us = host->state_times.running_us;
	out->state_times.ready_us = host->state_times.ready_us;
	out->state_times.blocked_us = host->state_times.blocked_us;
	out->state_times.suspended_us = host->state_times.suspended_us;
}

int lxp_ove_thread_snapshot_read(struct lxp_ove_thread_snapshot *snapshot,
				 struct lxp_thread_info *out, size_t max_count,
				 size_t *actual_count, lxp_ove_slot_lookup_t slot_lookup)
{
	if (actual_count)
		*actual_count = 0;
	if (!snapshot || (!out && max_count != 0))
		return LXP_ERR_INVALID_PARAM;

	size_t capacity = sizeof(snapshot->host) / sizeof(snapshot->host[0]);
	size_t limit = max_count < capacity ? max_count : capacity;
	size_t count = 0;
	int result = ove_thread_list(snapshot->host, limit, &count);
	if (count > limit)
		count = limit;
	for (size_t i = 0; i < count; i++)
		lxp_ove_thread_info_copy(&out[i], &snapshot->host[i], slot_lookup);
	if (actual_count)
		*actual_count = count;
	return result_to_lxp(result);
}
