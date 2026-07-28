/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-owned LXP host composition. Keep provider selection and the concrete
 * engine table out of applications.
 */

#include "ove/lxp_host.h"

#include "ove_config.h"

extern const lxp_os_ops_t g_lxp_host_engine;

#if defined(CONFIG_OVE_LINUX_NET)
extern const lxp_net_ops_t g_lxp_host_net_ops;
#define OVE_LXP_NET_OPS (&g_lxp_host_net_ops)
#else
#define OVE_LXP_NET_OPS NULL
#endif

#if defined(CONFIG_OVE_LINUX_DEV)
extern const lxp_display_ops_t g_lxp_host_display_ops;
#define OVE_LXP_DISPLAY_OPS (&g_lxp_host_display_ops)
#else
#define OVE_LXP_DISPLAY_OPS NULL
#endif

void ove_lxp_prepare_rootfs_access(const void *base, size_t len)
{
	if (g_lxp_host_engine.rootfs_window)
		g_lxp_host_engine.rootfs_window(base, len);
}

int ove_lxp_run(const lxp_run_config_t *config, const char *path, int argc,
		const char *const argv[])
{
	return lxp_run(&g_lxp_host_engine, OVE_LXP_NET_OPS, OVE_LXP_DISPLAY_OPS, config, path, argc,
		       argv);
}
