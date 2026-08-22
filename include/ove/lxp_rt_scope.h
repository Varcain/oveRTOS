/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LXP_RT_SCOPE_H
#define OVE_LXP_RT_SCOPE_H

#include <stddef.h>

#include "ove/lxp_launch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ove_lxp_rt_scope_write_fn)(const char *text);

/* Start the configured two-channel host real-time probe. */
int ove_lxp_rt_scope_start(ove_lxp_rt_scope_write_fn write_fn);

/** Bind the RT-scope proc provider to an otherwise caller-owned launch
 * configuration. Other launch policy is left unchanged. */
void ove_lxp_rt_scope_bind(ove_lxp_launch_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_RT_SCOPE_H */
