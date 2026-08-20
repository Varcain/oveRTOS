/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Private representation behind the public ove_lxp_host_t storage contract.
 */

#ifndef LXP_OVE_HOST_INTERNAL_H
#define LXP_OVE_HOST_INTERNAL_H

#include <stdint.h>

#include "lxp/lxp_host.h"
#include "ove/lxp_host.h"

#if defined(__GNUC__)
#define OVE_LXP_HOST_MAY_ALIAS_ __attribute__((__may_alias__))
#else
#define OVE_LXP_HOST_MAY_ALIAS_
#endif

typedef struct OVE_LXP_HOST_MAY_ALIAS_ ove_lxp_host_impl {
	lxp_file_t rootfs_files[OVE_LXP_ROOTFS_FILE_CAPACITY];
	char rootfs_names[OVE_LXP_ROOTFS_NAME_CAPACITY];
	lxp_host_t core;
	ove_netif_storage_t netif_storage;
	ove_netif_t netif;
	uint8_t netif_initialized;
	uint8_t netif_up;
} ove_lxp_host_impl_t;

#undef OVE_LXP_HOST_MAY_ALIAS_

_Static_assert(sizeof(lxp_file_t) == 4u * sizeof(uintptr_t),
	       "update the OVE LXP rootfs-entry storage ABI");
_Static_assert(sizeof(lxp_host_t) == 208u + 10u * sizeof(uintptr_t),
	       "update the OVE LXP fixed host storage ABI");
_Static_assert(sizeof(ove_lxp_host_impl_t) == OVE_LXP_HOST_STORAGE_SIZE,
	       "OVE LXP host storage size no longer matches its private representation");
_Static_assert(_Alignof(ove_lxp_host_impl_t) <= _Alignof(ove_lxp_host_t),
	       "OVE LXP host storage alignment is insufficient");

static inline ove_lxp_host_impl_t *ove_lxp_host_private(ove_lxp_host_t *host)
{
	return (ove_lxp_host_impl_t *)(void *)host->_opaque;
}

static inline const ove_lxp_host_impl_t *ove_lxp_host_private_const(const ove_lxp_host_t *host)
{
	return (const ove_lxp_host_impl_t *)(const void *)host->_opaque;
}

#endif /* LXP_OVE_HOST_INTERNAL_H */
