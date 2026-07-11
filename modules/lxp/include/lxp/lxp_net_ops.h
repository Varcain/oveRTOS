/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * The handle-based network port for the Linux personality. The personality's
 * socket + remote-fs cores reach the host TCP/IP stack ONLY through these ops:
 * the host owns socket storage and returns an opaque lxp_socket_t handle, so
 * the personality never embeds a backend-sized socket by value (that was the last
 * compile-time coupling to the RTOS storage layout). On oveRTOS the ops are filled
 * by backends/common/lxp_ove_adapter.c, which bridges to the ove_net HAL and
 * owns the storage pool. A non-oveRTOS host provides its own adapter over its own
 * stack. (Renamed to the neutral lxp_net_ops_t at the module-extraction rename.)
 */

#ifndef OVE_LINUX_NET_OPS_H
#define OVE_LINUX_NET_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "lxp/lxp_port.h" /* lxp_af_t / lxp_sock_type_t / lxp_sockaddr_t / lxp_netif_t (value types) */

#ifdef __cplusplus
extern "C" {
#endif

/* Max concurrent socket opens the personality pools (listener + clients). Shared
 * so the host adapter can size its storage pool to match. */
#ifndef LXP_NSOCK
#define LXP_NSOCK 24
#endif

/* struct lxp_net_ops (the handle-based network port), lxp_socket_t, lxp_sockaddr_t
 * and the address/socket value types all come from lxp_port.h. This header adds
 * only the module-internal binding below. */

/* The active network port. Set by the host (on oveRTOS: statically to the ove_net
 * adapter in backends/common/lxp_ove_adapter.c). The personality reads it; a
 * non-oveRTOS host may point it elsewhere. */
extern const struct lxp_net_ops *g_lxp_net_ops;

#ifdef __cplusplus
}
#endif

#endif /* OVE_LINUX_NET_OPS_H */
