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

#if defined(CONFIG_OVE_LINUX_FS)
extern const lxp_fs_ops_t g_lxp_host_fs_ops;
#define OVE_LXP_FS_OPS (&g_lxp_host_fs_ops)
#else
#define OVE_LXP_FS_OPS NULL
#endif

#if defined(CONFIG_OVE_LINUX_BLOCK)
extern const lxp_block_ops_t g_lxp_host_block_ops;
#define OVE_LXP_BLOCK_OPS (&g_lxp_host_block_ops)
#else
#define OVE_LXP_BLOCK_OPS NULL
#endif

int ove_lxp_host_init_cpio(lxp_host_t *host, const void *rootfs_image, size_t rootfs_image_size,
			   lxp_file_t *rootfs_storage, int rootfs_capacity,
			   char *rootfs_name_storage, size_t rootfs_name_capacity)
{
	const lxp_host_config_t config = {
		.os_ops = &g_lxp_host_engine,
		.net_ops = OVE_LXP_NET_OPS,
		.display_ops = OVE_LXP_DISPLAY_OPS,
		.fs_ops = OVE_LXP_FS_OPS,
		.block_ops = OVE_LXP_BLOCK_OPS,
		.rootfs_image = rootfs_image,
		.rootfs_image_size = rootfs_image_size,
		.rootfs_storage = rootfs_storage,
		.rootfs_capacity = rootfs_capacity,
		.rootfs_name_storage = rootfs_name_storage,
		.rootfs_name_capacity = rootfs_name_capacity,
	};
	return lxp_host_init_cpio(host, &config);
}

int ove_lxp_host_run(const lxp_host_t *host, const lxp_launch_config_t *config, const char *path,
		     int argc, const char *const argv[])
{
	return lxp_host_run(host, config, path, argc, argv);
}
