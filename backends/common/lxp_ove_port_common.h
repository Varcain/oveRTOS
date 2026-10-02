/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LXP_OVE_PORT_COMMON_H
#define LXP_OVE_PORT_COMMON_H

#include "lxp/lxp_config.h"
#include "lxp/ports/cortex_m.h"
#include "lxp_ove_thread_adapter.h"
#include "ove/build.h"
#include "ove/time.h"

/*
 * Initializer fragments for the .common configuration each oveRTOS host gives
 * LXP's Cortex-M port. Where the storage lives, its memory attributes and the
 * memory-contract validator stay engine- and board-specific; these fragments
 * fill what every engine must fill the same way.
 */

/* The guest storage: LXP_NREG program regions and dynamic pools and LXP_NSLOT
 * exec captures, with strides and counts taken from LXP's own constants. */
#define LXP_OVE_PORT_STORAGE(programs, pools, captures)                               \
	.program_regions = (programs), .program_region_stride = LXP_PROG_REGION_SIZE, \
	.program_region_count = LXP_NREG, .dynamic_pools = (pools),                   \
	.dynamic_pool_stride = LXP_DYN_POOL_SIZE, .dynamic_pool_count = LXP_NREG,     \
	.exec_captures = (captures), .exec_capture_count = LXP_NSLOT

/* oveRTOS time, memory statistics and thread reporting, plus the engine's
 * uname version string and memory-contract validator. */
#define LXP_OVE_PORT_SERVICES(thread_list_fn, version, validator)                                \
	.time_us = ove_time_get_us, .time_ns = ove_time_get_ns, .thread_list = (thread_list_fn), \
	.mem_stats = lxp_ove_mem_stats_read, .system_version = (version),                        \
	.validate_memory_contract = (validator)

/* uname's version field: the engine's kernel version, then the oveRTOS and LXP
 * revisions. Linux's utsname field holds 65 bytes including the terminator. */
#define LXP_OVE_SYSTEM_VERSION(kernel) \
	kernel " ove-" OVE_BUILD_OVERTOS_REV " lxp-" OVE_BUILD_LXP_REV
#define LXP_OVE_SYSTEM_VERSION_MAX 65u

#endif /* LXP_OVE_PORT_COMMON_H */
