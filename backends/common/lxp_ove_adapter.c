/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * oveRTOS host adapter for the lxp network port (struct lxp_net_ops). It bridges
 * the handle-based, module-owned lxp types to the ove_net HAL (lwIP / NuttX net /
 * Zephyr net / POSIX sockets) and OWNS the socket-storage pool — the backend-sized
 * ove_socket_storage_t the personality no longer embeds. The lxp value types
 * (lxp_sockaddr_t / lxp_af_t) are field-converted to their ove_net equivalents at
 * this boundary (identical values, but lxp_af_t is a fixed uint8_t vs ove_af_t's
 * enum, so a field copy, not a cast). Engine-agnostic: same adapter for all three
 * RTOS engines and the host test.
 *
 * The exported provider table is passed explicitly to lxp_run(), which publishes
 * it only for the duration of the personality run. A non-oveRTOS host supplies
 * its own table.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_NET)

#include "ove/net.h"
#include "lxp/lxp_net_ops.h"

#include <string.h>

#define LXP_ADAPTER_NSOCK (LXP_NSOCK + 4)

/* The opaque handle the module holds: a pool entry carrying the backend-sized
 * storage and the resulting ove_socket handle. */
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

/* ---- lxp <-> ove address conversion (same fields + values) ------------------ */
static void to_ove(const lxp_sockaddr_t *a, ove_sockaddr_t *o)
{
	o->family = (ove_af_t)a->family;
	o->port = a->port;
	memcpy(o->addr, a->addr, sizeof(o->addr));
}
static void from_ove(const ove_sockaddr_t *o, lxp_sockaddr_t *a)
{
	a->family = (lxp_af_t)o->family;
	a->port = o->port;
	memcpy(a->addr, o->addr, sizeof(a->addr));
}

static int a_open(lxp_af_t af, lxp_sock_type_t type, int proto, lxp_socket_t *out)
{
	struct lxp_socket *s = slot_alloc();
	if (!s)
		return LXP_ERR_NO_MEMORY;
	int r = ove_socket_open_ex(&s->h, &s->st, (ove_af_t)af, (ove_sock_type_t)type, proto);
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
		return LXP_ERR_NO_MEMORY;
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
static int a_connect(lxp_socket_t s, const lxp_sockaddr_t *a, uint64_t t)
{
	ove_sockaddr_t oa;
	to_ove(a, &oa);
	return ove_socket_connect(s->h, &oa, t);
}
static int a_bind(lxp_socket_t s, const lxp_sockaddr_t *a)
{
	ove_sockaddr_t oa;
	to_ove(a, &oa);
	return ove_socket_bind(s->h, &oa);
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
		    const lxp_sockaddr_t *dst)
{
	ove_sockaddr_t oa;
	to_ove(dst, &oa);
	return ove_socket_sendto(s->h, d, n, sent, &oa);
}
static int a_recvfrom(lxp_socket_t s, void *b, size_t n, size_t *got, lxp_sockaddr_t *src,
		      uint64_t t)
{
	ove_sockaddr_t oa;
	int r = ove_socket_recvfrom(s->h, b, n, got, &oa, t);
	if (r == OVE_OK && src)
		from_ove(&oa, src);
	return r;
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
static int a_getsockname(lxp_socket_t s, lxp_sockaddr_t *a)
{
	ove_sockaddr_t oa;
	int r = ove_socket_getsockname(s->h, &oa);
	if (r == OVE_OK)
		from_ove(&oa, a);
	return r;
}
static int a_getpeername(lxp_socket_t s, lxp_sockaddr_t *a)
{
	ove_sockaddr_t oa;
	int r = ove_socket_getpeername(s->h, &oa);
	if (r == OVE_OK)
		from_ove(&oa, a);
	return r;
}
static int a_get_error(lxp_socket_t s)
{
	return ove_socket_get_error(s->h);
}

/* netif ops: the module holds the interface as an lxp_netif_t, which on oveRTOS is
 * really the ove_netif_t handed in via lxp_net_set_netif — cast back here. */
static int a_netif_get_addr(lxp_netif_t nif, lxp_sockaddr_t *ip, lxp_sockaddr_t *gw,
			    lxp_sockaddr_t *nm)
{
	ove_sockaddr_t oip = {0}, ogw = {0}, onm = {0};
	int r = ove_netif_get_addr((ove_netif_t)nif, &oip, &ogw, &onm);
	if (ip)
		from_ove(&oip, ip);
	if (gw)
		from_ove(&ogw, gw);
	if (nm)
		from_ove(&onm, nm);
	return r;
}
static int a_netif_get_hwaddr(lxp_netif_t nif, uint8_t mac[6])
{
	return ove_netif_get_hwaddr((ove_netif_t)nif, mac);
}
static int a_netif_get_flags(lxp_netif_t nif, unsigned *flags)
{
	return ove_netif_get_flags((ove_netif_t)nif, flags);
}
static int a_netif_set_addr(lxp_netif_t nif, const lxp_sockaddr_t *ip, const lxp_sockaddr_t *nm,
			    const lxp_sockaddr_t *gw)
{
	ove_sockaddr_t oip, onm, ogw;
	if (ip)
		to_ove(ip, &oip);
	if (nm)
		to_ove(nm, &onm);
	if (gw)
		to_ove(gw, &ogw);
	return ove_netif_set_addr((ove_netif_t)nif, ip ? &oip : NULL, nm ? &onm : NULL,
				  gw ? &ogw : NULL);
}
static int a_netif_set_up(lxp_netif_t nif, int up)
{
	return ove_netif_set_up((ove_netif_t)nif, up);
}

const struct lxp_net_ops g_lxp_host_net_ops = {
	.abi_version = LXP_NET_OPS_ABI_VERSION,
	.struct_size = sizeof(struct lxp_net_ops),
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
#if defined(CONFIG_OVE_RTOS_FREERTOS)
	/* freertos_net.c posts lxp_sock_kick() after each delivered RX batch. */
	.capabilities = LXP_NET_CAP_SOCKET_READY_EVENT,
#endif
};

/* Active only while lxp_run() publishes a provider. */
const struct lxp_net_ops *g_lxp_net_ops;

#endif /* CONFIG_OVE_LINUX_NET */
