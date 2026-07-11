/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * The handle-based network port for the Linux personality. The personality's
 * socket + remote-fs cores reach the host TCP/IP stack ONLY through these ops:
 * the host owns socket storage and returns an opaque ove_lnx_socket_t handle, so
 * the personality never embeds a backend-sized socket by value (that was the last
 * compile-time coupling to the RTOS storage layout). On oveRTOS the ops are filled
 * by backends/common/ove_lnx_ove_adapter.c, which bridges to the ove_net HAL and
 * owns the storage pool. A non-oveRTOS host provides its own adapter over its own
 * stack. (Renamed to the neutral lxp_net_ops_t at the module-extraction rename.)
 */

#ifndef OVE_LINUX_NET_OPS_H
#define OVE_LINUX_NET_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "ove/net.h" /* ove_af_t / ove_sock_type_t / ove_sockaddr_t / ove_netif_t (value types) */

#ifdef __cplusplus
extern "C" {
#endif

/* Max concurrent socket opens the personality pools (listener + clients). Shared
 * so the host adapter can size its storage pool to match. */
#ifndef OVE_LNX_NSOCK
#define OVE_LNX_NSOCK 24
#endif

/* Host-owned opaque socket handle: the module holds these, the host allocates the
 * backing storage inside sock_open / sock_accept. */
typedef struct ove_lnx_socket *ove_lnx_socket_t;

/* The network port. All calls run on the coordinator thread (the park/retry model
 * serialises them), so a host implementation needs no internal locking. Return
 * values are ove_net status codes (OVE_OK / OVE_ERR_*), which the caller maps to
 * a guest errno; a would-block is OVE_ERR_TIMEOUT. */
struct ove_lnx_net_ops {
	/* Allocate storage + open a socket; *out receives the handle on success. */
	int (*sock_open)(ove_af_t af, ove_sock_type_t type, int proto, ove_lnx_socket_t *out);
	/* Accept a pending connection: allocate storage for the client, *out = handle. */
	int (*sock_accept)(ove_lnx_socket_t listener, ove_lnx_socket_t *out, uint64_t timeout_ns);
	void (*sock_close)(ove_lnx_socket_t s);
	int (*sock_connect)(ove_lnx_socket_t s, const ove_sockaddr_t *a, uint64_t timeout_ns);
	int (*sock_bind)(ove_lnx_socket_t s, const ove_sockaddr_t *a);
	int (*sock_listen)(ove_lnx_socket_t s, int backlog);
	int (*sock_send)(ove_lnx_socket_t s, const void *d, size_t n, size_t *sent);
	int (*sock_recv)(ove_lnx_socket_t s, void *b, size_t n, size_t *got, uint64_t timeout_ns);
	int (*sock_sendto)(ove_lnx_socket_t s, const void *d, size_t n, size_t *sent,
			   const ove_sockaddr_t *dst);
	int (*sock_recvfrom)(ove_lnx_socket_t s, void *b, size_t n, size_t *got, ove_sockaddr_t *src,
			     uint64_t timeout_ns);
	int (*sock_set_nonblock)(ove_lnx_socket_t s, int nb);
	int (*sock_poll)(ove_lnx_socket_t s, unsigned events, unsigned *revents,
			 uint64_t timeout_ns);
	int (*sock_shutdown)(ove_lnx_socket_t s, int how);
	int (*sock_getsockname)(ove_lnx_socket_t s, ove_sockaddr_t *a);
	int (*sock_getpeername)(ove_lnx_socket_t s, ove_sockaddr_t *a);
	int (*sock_get_error)(ove_lnx_socket_t s);
	/* Interface introspection / config for the SIOC* ioctls (ifconfig / route).
	 * The netif handle is the one the personality holds via ove_lnx_sock_set_netif. */
	int (*netif_get_addr)(ove_netif_t nif, ove_sockaddr_t *ip, ove_sockaddr_t *gw,
			      ove_sockaddr_t *nm);
	int (*netif_get_hwaddr)(ove_netif_t nif, uint8_t mac[6]);
	int (*netif_get_flags)(ove_netif_t nif, unsigned *flags);
	int (*netif_set_addr)(ove_netif_t nif, const ove_sockaddr_t *ip, const ove_sockaddr_t *nm,
			      const ove_sockaddr_t *gw);
	int (*netif_set_up)(ove_netif_t nif, int up);
};

/* The active network port. Set by the host (on oveRTOS: statically to the ove_net
 * adapter). The personality reads it; a non-oveRTOS host may point it elsewhere. */
extern const struct ove_lnx_net_ops *g_ove_lnx_net_ops;

#ifdef __cplusplus
}
#endif

#endif /* OVE_LINUX_NET_OPS_H */
