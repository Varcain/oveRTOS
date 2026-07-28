/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-owned composition facade for the LXP Linux personality.
 */
#ifndef OVE_LXP_HOST_H
#define OVE_LXP_HOST_H

#include <stddef.h>

#include "lxp/lxp_run.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Give the selected engine access to an external rootfs before the application
 * parses it. Engines without a special mapping requirement treat this as a no-op.
 */
void ove_lxp_prepare_rootfs_access(const void *base, size_t len);

/**
 * Run a guest with the OS, network, and display providers selected by the
 * oveRTOS build. Applications own only the per-run guest configuration.
 */
int ove_lxp_run(const lxp_run_config_t *config, const char *path, int argc,
		const char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_HOST_H */
