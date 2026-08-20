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
#include <stdint.h>

#include "lxp/lxp_host.h"
#include "ove/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Product-selected 9P topology translated into LXP's immutable host contract. */
typedef struct ove_lxp_netfs_config {
	const char *mountpoint;
	const char *server_ipv4;
	uint16_t port;
	const char *aname;
	const char *uname;
} ove_lxp_netfs_config_t;

/** One-time inputs for an oveRTOS-owned LXP host. Referenced values are consumed
 * during init; LXP copies rootfs metadata and netfs strings into host storage. */
typedef struct ove_lxp_host_config {
	const void *rootfs_image;
	size_t rootfs_image_size;
	lxp_file_t *rootfs_storage;
	int rootfs_capacity;
	char *rootfs_name_storage;
	size_t rootfs_name_capacity;
	const ove_netif_config_t *netif_config;
	/** Bounded best-effort wait for an address after interface bring-up. */
	uint32_t netif_address_wait_ms;
	const ove_lxp_netfs_config_t *netfs_config;
} ove_lxp_host_config_t;

/** oveRTOS-owned native resources plus LXP's immutable host. Do not copy an
 * initialized object. Calls to ove_lxp_host_run() must be sequential. */
typedef struct ove_lxp_host {
	lxp_host_t core;
	ove_netif_storage_t netif_storage;
	ove_netif_t netif;
	uint8_t netif_initialized;
	uint8_t netif_up;
} ove_lxp_host_t;

/** Bring up optional native networking, then parse and publish the rootfs. */
int ove_lxp_host_init_cpio(ove_lxp_host_t *host, const ove_lxp_host_config_t *config);

/** Release native resources after the last run. Safe after a failed init. */
void ove_lxp_host_deinit(ove_lxp_host_t *host);

/** Query the interface owned by @p host, or return OVE_ERR_NOT_SUPPORTED. */
int ove_lxp_host_netif_get_addr(const ove_lxp_host_t *host, ove_sockaddr_t *ip,
				ove_sockaddr_t *gateway, ove_sockaddr_t *netmask);

/** Launch a guest through an initialized host. Rootfs and provider composition
 * remain owned by LXP; the application supplies per-launch policy only. */
int ove_lxp_host_run(const ove_lxp_host_t *host, const lxp_launch_config_t *config,
		     const char *path, int argc, const char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_HOST_H */
