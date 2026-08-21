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

static uint32_t thread_fields_to_lxp(uint32_t fields)
{
	uint32_t mapped = 0;
#define MAP_FIELD(name)                                         \
	do {                                                    \
		if (fields & OVE_THREAD_INFO_VALID_##name)      \
			mapped |= LXP_THREAD_INFO_VALID_##name; \
	} while (0)
	MAP_FIELD(STACK_USED);
	MAP_FIELD(STACK_SIZE);
	MAP_FIELD(CPU_PERCENT);
	MAP_FIELD(RUNNING_TIME);
	MAP_FIELD(READY_TIME);
	MAP_FIELD(BLOCKED_TIME);
	MAP_FIELD(SUSPENDED_TIME);
#undef MAP_FIELD
	return mapped;
}

void lxp_ove_thread_info_copy(struct lxp_thread_info *out, const struct ove_thread_info *host,
			      lxp_ove_slot_lookup_t slot_lookup)
{
	out->name = host->name;
	out->identity = host->identity;
	out->lxp_slot = slot_lookup ? slot_lookup(host->identity) : LXP_THREAD_SLOT_NONE;
	out->state = thread_state_to_lxp(host->state);
	out->priority = host->priority;
	out->valid_fields = thread_fields_to_lxp(host->valid_fields);
	out->stack_used = (out->valid_fields & LXP_THREAD_INFO_VALID_STACK_USED) ? host->stack_used
										 : 0;
	out->stack_size = (out->valid_fields & LXP_THREAD_INFO_VALID_STACK_SIZE) ? host->stack_size
										 : 0;
	out->cpu_percent_x100 = (out->valid_fields & LXP_THREAD_INFO_VALID_CPU_PERCENT)
					? host->cpu_percent_x100
					: 0;
	out->state_times.running_us = (out->valid_fields & LXP_THREAD_INFO_VALID_RUNNING_TIME)
					      ? host->state_times.running_us
					      : 0;
	out->state_times.ready_us = (out->valid_fields & LXP_THREAD_INFO_VALID_READY_TIME)
					    ? host->state_times.ready_us
					    : 0;
	out->state_times.blocked_us = (out->valid_fields & LXP_THREAD_INFO_VALID_BLOCKED_TIME)
					      ? host->state_times.blocked_us
					      : 0;
	out->state_times.suspended_us = (out->valid_fields & LXP_THREAD_INFO_VALID_SUSPENDED_TIME)
						? host->state_times.suspended_us
						: 0;
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
