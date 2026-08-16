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

#include "lxp/lxp_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Parse and publish one immutable CPIO rootfs using the providers selected by
 * the oveRTOS build. The caller supplies only product-selected image/storage. */
int ove_lxp_host_init_cpio(lxp_host_t *host, const void *rootfs_image, size_t rootfs_image_size,
			   lxp_file_t *rootfs_storage, int rootfs_capacity,
			   char *rootfs_name_storage, size_t rootfs_name_capacity);

/** Launch a guest through an initialized host. Rootfs and provider composition
 * remain owned by LXP; the application supplies per-launch policy only. */
int ove_lxp_host_run(const lxp_host_t *host, const lxp_launch_config_t *config, const char *path,
		     int argc, const char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_HOST_H */
