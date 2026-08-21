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

#include "ove/lxp_launch.h"
#include "ove/net.h"
#include "ove_config.h"

#if !defined(CONFIG_OVE_LINUX_ROOTFS_FILE_CAPACITY) || \
	!defined(CONFIG_OVE_LINUX_ROOTFS_NAME_CAPACITY)
#error "LXP host rootfs workspace capacities are missing from ove_config.h"
#endif

#define OVE_LXP_ROOTFS_FILE_CAPACITY CONFIG_OVE_LINUX_ROOTFS_FILE_CAPACITY
#define OVE_LXP_ROOTFS_NAME_CAPACITY CONFIG_OVE_LINUX_ROOTFS_NAME_CAPACITY

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
 * during init; the host owns rootfs metadata storage and LXP copies netfs strings. */
typedef struct ove_lxp_host_config {
	const void *rootfs_image;
	size_t rootfs_image_size;
	const ove_netif_config_t *netif_config;
	/** Bounded best-effort wait for an address after interface bring-up. */
	uint32_t netif_address_wait_ms;
	const ove_lxp_netfs_config_t *netfs_config;
} ove_lxp_host_config_t;

/* Keep this caller-owned object exactly sized without exposing canonical LXP
 * types. One rootfs entry occupies four pointer-width words on the supported
 * ABIs. The fixed-state reserve covers the immutable canonical host record;
 * private compile-time assertions fail if either storage ABI changes. */
#define OVE_LXP_ALIGN_UP_(value, alignment) \
	(((value) + (alignment) - 1u) / (alignment) * (alignment))
#define OVE_LXP_ROOTFS_STORAGE_BYTES_ (OVE_LXP_ROOTFS_FILE_CAPACITY * 4u * sizeof(uintptr_t))
#define OVE_LXP_CORE_STORAGE_BYTES_ (208u + 10u * sizeof(uintptr_t))
#define OVE_LXP_CORE_OFFSET_                                                            \
	OVE_LXP_ALIGN_UP_(OVE_LXP_ROOTFS_STORAGE_BYTES_ + OVE_LXP_ROOTFS_NAME_CAPACITY, \
			  sizeof(uintptr_t))
#define OVE_LXP_NETIF_OFFSET_ (OVE_LXP_CORE_OFFSET_ + OVE_LXP_CORE_STORAGE_BYTES_)
#define OVE_LXP_HANDLE_OFFSET_ \
	OVE_LXP_ALIGN_UP_(OVE_LXP_NETIF_OFFSET_ + sizeof(ove_netif_storage_t), sizeof(uintptr_t))

enum {
	OVE_LXP_HOST_STORAGE_SIZE = OVE_LXP_ALIGN_UP_(
		OVE_LXP_HANDLE_OFFSET_ + sizeof(ove_netif_t) + 2u, sizeof(uintptr_t)),
};

#undef OVE_LXP_HANDLE_OFFSET_
#undef OVE_LXP_NETIF_OFFSET_
#undef OVE_LXP_CORE_OFFSET_
#undef OVE_LXP_CORE_STORAGE_BYTES_
#undef OVE_LXP_ROOTFS_STORAGE_BYTES_
#undef OVE_LXP_ALIGN_UP_

/** Opaque oveRTOS-owned rootfs workspace, native resources, and LXP host.
 *
 * The pointer-width member supplies the alignment required by the private
 * representation. Runtime reset deliberately does not clear the large rootfs
 * workspace. Do not inspect or copy an initialized object. Calls to
 * ove_lxp_host_run() must be sequential.
 */
typedef union ove_lxp_host {
	uintptr_t _alignment;
	uint8_t _opaque[OVE_LXP_HOST_STORAGE_SIZE];
} ove_lxp_host_t;

/**
 * Bring up the build-configured Linux host.
 *
 * Rootfs placement, native interface topology, and optional netfs topology are
 * taken from CONFIG_OVE_LINUX_* settings. This is the normal application entry
 * point; use ove_lxp_host_init_cpio() only when supplying a runtime-selected
 * rootfs or network composition.
 */
int ove_lxp_host_init(ove_lxp_host_t *host);

/** Bring up optional native networking, then parse and publish the rootfs. */
int ove_lxp_host_init_cpio(ove_lxp_host_t *host, const ove_lxp_host_config_t *config);

/** Release native resources after the last run. Safe after a failed init. */
void ove_lxp_host_deinit(ove_lxp_host_t *host);

/** Query the interface owned by @p host, or return OVE_ERR_NOT_SUPPORTED. */
int ove_lxp_host_netif_get_addr(const ove_lxp_host_t *host, ove_sockaddr_t *ip,
				ove_sockaddr_t *gateway, ove_sockaddr_t *netmask);

/** Launch a guest through an initialized host. Rootfs and provider composition
 * remain owned by LXP; the application supplies per-launch policy only. */
int ove_lxp_host_run(const ove_lxp_host_t *host, const ove_lxp_launch_config_t *config,
		     const char *path, int argc, const char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_HOST_H */
