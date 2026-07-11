/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * oveRTOS host adapter for the Linux personality's network port
 * (struct lxp_net_ops). It bridges the handle-based port to the ove_net HAL
 * (lwIP / NuttX net / Zephyr net / POSIX sockets) and OWNS the socket-storage
 * pool — the backend-sized ove_socket_storage_t that the personality no longer
 * embeds. Engine-agnostic: it calls only ove_socket_* / ove_netif_*, so the same
 * adapter serves all three RTOS engines and the host test build.
 *
 * g_lxp_net_ops is statically pointed here, so merely linking this TU wires
 * the personality to the ove_net stack (no init call). A non-oveRTOS host links
 * its own adapter instead.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_NET)

#include "ove/net.h"
#include "ove/linux/net_ops.h"

/* Enough storage for the personality's socket pool (LXP_NSOCK) plus the
 * remote-fs client socket and a little slack. */
#define LXP_ADAPTER_NSOCK (LXP_NSOCK + 4)

/* The opaque handle the personality holds: a pool entry that carries the
 * backend-sized storage and the resulting ove_socket handle. */
struct lxp_socket {
	ove_socket_storage_t st;
	ove_socket_t h;
	uint8_t used;
};

static struct lxp_socket g_pool[LXP_ADAPTER_NSOCK];

static struct lxp_socket *slot_alloc(void)
{
	for (int i = 0; i < LXP_ADAPTER_NSOCK; i++)
		if (!g_pool[i].used) {
			g_pool[i].used = 1;
			return &g_pool[i];
		}
	return NULL;
}

static int a_open(ove_af_t af, ove_sock_type_t type, int proto, lxp_socket_t *out)
{
	struct lxp_socket *s = slot_alloc();
	if (!s)
		return OVE_ERR_NO_MEMORY;
	int r = ove_socket_open_ex(&s->h, &s->st, af, type, proto);
	if (r != OVE_OK) {
		s->used = 0;
		return r;
	}
	*out = s;
	return OVE_OK;
}

static int a_accept(lxp_socket_t listener, lxp_socket_t *out, uint64_t timeout_ns)
{
	struct lxp_socket *s = slot_alloc();
	if (!s)
		return OVE_ERR_NO_MEMORY;
	int r = ove_socket_accept(listener->h, &s->h, &s->st, timeout_ns);
	if (r != OVE_OK) {
		s->used = 0; /* also the OVE_ERR_TIMEOUT (no pending connection) path */
		return r;
	}
	*out = s;
	return OVE_OK;
}

static void a_close(lxp_socket_t s)
{
	if (!s)
		return;
	ove_socket_close(s->h);
	s->used = 0;
}

static int a_connect(lxp_socket_t s, const ove_sockaddr_t *a, uint64_t t)
{
	return ove_socket_connect(s->h, a, t);
}
static int a_bind(lxp_socket_t s, const ove_sockaddr_t *a)
{
	return ove_socket_bind(s->h, a);
}
static int a_listen(lxp_socket_t s, int backlog)
{
	return ove_socket_listen(s->h, backlog);
}
static int a_send(lxp_socket_t s, const void *d, size_t n, size_t *sent)
{
	return ove_socket_send(s->h, d, n, sent);
}
static int a_recv(lxp_socket_t s, void *b, size_t n, size_t *got, uint64_t t)
{
	return ove_socket_recv(s->h, b, n, got, t);
}
static int a_sendto(lxp_socket_t s, const void *d, size_t n, size_t *sent,
		    const ove_sockaddr_t *dst)
{
	return ove_socket_sendto(s->h, d, n, sent, dst);
}
static int a_recvfrom(lxp_socket_t s, void *b, size_t n, size_t *got, ove_sockaddr_t *src,
		      uint64_t t)
{
	return ove_socket_recvfrom(s->h, b, n, got, src, t);
}
static int a_set_nonblock(lxp_socket_t s, int nb)
{
	return ove_socket_set_nonblock(s->h, nb);
}
static int a_poll(lxp_socket_t s, unsigned events, unsigned *revents, uint64_t t)
{
	return ove_socket_poll(s->h, events, revents, t);
}
static int a_shutdown(lxp_socket_t s, int how)
{
	return ove_socket_shutdown(s->h, how);
}
static int a_getsockname(lxp_socket_t s, ove_sockaddr_t *a)
{
	return ove_socket_getsockname(s->h, a);
}
static int a_getpeername(lxp_socket_t s, ove_sockaddr_t *a)
{
	return ove_socket_getpeername(s->h, a);
}
static int a_get_error(lxp_socket_t s)
{
	return ove_socket_get_error(s->h);
}

/* netif ops: the handle IS an ove_netif_t (the personality holds it directly). */
static int a_netif_get_addr(ove_netif_t nif, ove_sockaddr_t *ip, ove_sockaddr_t *gw,
			    ove_sockaddr_t *nm)
{
	return ove_netif_get_addr(nif, ip, gw, nm);
}
static int a_netif_get_hwaddr(ove_netif_t nif, uint8_t mac[6])
{
	return ove_netif_get_hwaddr(nif, mac);
}
static int a_netif_get_flags(ove_netif_t nif, unsigned *flags)
{
	return ove_netif_get_flags(nif, flags);
}
static int a_netif_set_addr(ove_netif_t nif, const ove_sockaddr_t *ip, const ove_sockaddr_t *nm,
			    const ove_sockaddr_t *gw)
{
	return ove_netif_set_addr(nif, ip, nm, gw);
}
static int a_netif_set_up(ove_netif_t nif, int up)
{
	return ove_netif_set_up(nif, up);
}

static const struct lxp_net_ops g_ove_adapter_net_ops = {
	.sock_open = a_open,
	.sock_accept = a_accept,
	.sock_close = a_close,
	.sock_connect = a_connect,
	.sock_bind = a_bind,
	.sock_listen = a_listen,
	.sock_send = a_send,
	.sock_recv = a_recv,
	.sock_sendto = a_sendto,
	.sock_recvfrom = a_recvfrom,
	.sock_set_nonblock = a_set_nonblock,
	.sock_poll = a_poll,
	.sock_shutdown = a_shutdown,
	.sock_getsockname = a_getsockname,
	.sock_getpeername = a_getpeername,
	.sock_get_error = a_get_error,
	.netif_get_addr = a_netif_get_addr,
	.netif_get_hwaddr = a_netif_get_hwaddr,
	.netif_get_flags = a_netif_get_flags,
	.netif_set_addr = a_netif_set_addr,
	.netif_set_up = a_netif_set_up,
};

/* Statically wire the personality to this adapter (no init call needed). A future
 * lxp_run(net) will assign this pointer explicitly instead. */
const struct lxp_net_ops *g_lxp_net_ops = &g_ove_adapter_net_ops;

#endif /* CONFIG_OVE_LINUX_NET */
