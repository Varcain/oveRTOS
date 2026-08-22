/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LXP_OVE_THREAD_ADAPTER_H
#define LXP_OVE_THREAD_ADAPTER_H

#include "lxp/lxp_stats.h"
#include "ove/thread.h"

typedef int32_t (*lxp_ove_slot_lookup_t)(uintptr_t identity);

/*
 * The RTOS seam owns one snapshot workspace. Keeping the differently typed
 * oveRTOS records here prevents either API from depending on the other's
 * representation.
 */
struct lxp_ove_thread_snapshot {
	struct ove_thread_info host[LXP_MAX_KTHREAD];
};

/** Convert one host record field-by-field and assign its owning LXP slot. */
void lxp_ove_thread_info_copy(struct lxp_thread_info *out, const struct ove_thread_info *host,
			      lxp_ove_slot_lookup_t slot_lookup);

/** Read an oveRTOS thread snapshot and translate it into the LXP contract. */
int lxp_ove_thread_snapshot_read(struct lxp_ove_thread_snapshot *snapshot,
				 struct lxp_thread_info *out, size_t max_count,
				 size_t *actual_count, lxp_ove_slot_lookup_t slot_lookup);

/** Read and translate the host allocator statistics exposed to LXP. */
int lxp_ove_mem_stats_read(struct lxp_mem_stats *out);

#endif /* LXP_OVE_THREAD_ADAPTER_H */
