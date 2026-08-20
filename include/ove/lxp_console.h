/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS system-console provider for LXP launches.
 */
#ifndef OVE_LXP_CONSOLE_H
#define OVE_LXP_CONSOLE_H

#include "ove/lxp_launch.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the console transport used by the Linux personality. */
int ove_lxp_console_init(void);

/** Fill only the console-related fields of an otherwise caller-owned launch
 * configuration. Diagnostics, environment, display, and workload policy are
 * left unchanged. */
void ove_lxp_console_bind(ove_lxp_launch_config_t *config);

/** Write a NUL-terminated host diagnostic through the personality console. */
void ove_lxp_console_write(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_CONSOLE_H */
