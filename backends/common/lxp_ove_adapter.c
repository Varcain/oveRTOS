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
#if defined(CONFIG_OVE_NET_RX_READY_NOTIFY)
#include "ove_net_ready.h"
#include "lxp/lxp_net.h"
#endif
#include "lxp/lxp_config.h"
#include "lxp/lxp_net_ops.h"

#include <stdint.h>
#include <string.h>

/*
 * The socket core owns at most LXP_NSOCK handles. Netfs has one independent
 * transport handle, so it contributes exactly one slot when compiled in.
 */
#define LXP_ADAPTER_NSOCK (LXP_NSOCK + LXP_ENABLE_NETFS)

/* The opaque handle the module holds: a pool entry carrying the backend-sized
 * storage and the resulting ove_socket handle. */
struct lxp_socket {
	ove_socket_storage_t st;
	ove_socket_t h;
	uint8_t used;
};

static struct lxp_socket g_pool[LXP_ADAPTER_NSOCK];
static uint8_t g_run_active;

#if defined(CONFIG_OVE_NET_RX_READY_NOTIFY)
static unsigned g_open_sockets;

static void socket_ready(void)
{
	lxp_sock_kick();
}
#endif

static struct lxp_socket *slot_lookup(lxp_socket_t handle)
{
	uintptr_t addr = (uintptr_t)handle;
	uintptr_t base = (uintptr_t)&g_pool[0];
	size_t offset;

	if (!handle || addr < base || addr >= base + sizeof(g_pool))
		return NULL;
	offset = (size_t)(addr - base);
	if (offset % sizeof(g_pool[0]) != 0)
		return NULL;
	struct lxp_socket *socket = &g_pool[offset / sizeof(g_pool[0])];
	return socket->used ? socket : NULL;
}

static struct lxp_socket *slot_alloc(void)
{
	if (!g_run_active)
		return NULL;
	for (int i = 0; i < LXP_ADAPTER_NSOCK; i++)
		if (!g_pool[i].used) {
			g_pool[i].used = 1;
			return &g_pool[i];
		}
	return NULL;
}

static int slot_publish(struct lxp_socket *s, lxp_socket_t *out)
{
#if defined(CONFIG_OVE_NET_RX_READY_NOTIFY)
	if (g_open_sockets == 0) {
		int rc = ove_net_ready_subscribe(socket_ready);
		if (rc != OVE_OK) {
			ove_socket_close(s->h);
			memset(s, 0, sizeof(*s));
			return rc;
		}
	}
	g_open_sockets++;
#endif
	*out = s;
	return OVE_OK;
}

static void slot_close(struct lxp_socket *socket)
{
	ove_socket_close(socket->h);
	memset(socket, 0, sizeof(*socket));
#if defined(CONFIG_OVE_NET_RX_READY_NOTIFY)
	if (g_open_sockets > 0)
		g_open_sockets--;
	if (g_open_sockets == 0)
		ove_net_ready_unsubscribe(socket_ready);
#endif
}

static void pool_reset(void)
{
	for (int i = 0; i < LXP_ADAPTER_NSOCK; i++)
		if (g_pool[i].used)
			slot_close(&g_pool[i]);
	memset(g_pool, 0, sizeof(g_pool));
#if defined(CONFIG_OVE_NET_RX_READY_NOTIFY)
	g_open_sockets = 0;
	ove_net_ready_unsubscribe(socket_ready);
#endif
}

static int a_run_begin(void)
{
	if (g_run_active)
		return OVE_ERR_WOULD_BLOCK;
	pool_reset();
	g_run_active = 1;
	return OVE_OK;
}

static void a_run_end(void)
{
	if (!g_run_active)
		return;
	pool_reset();
	g_run_active = 0;
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
	if (!g_run_active || !out)
		return OVE_ERR_INVALID_PARAM;
	struct lxp_socket *s = slot_alloc();
	if (!s)
		return LXP_ERR_NO_MEMORY;
	int r = ove_socket_open_ex(&s->h, &s->st, (ove_af_t)af, (ove_sock_type_t)type, proto);
	if (r != OVE_OK) {
		s->used = 0;
		return r;
	}
	return slot_publish(s, out);
}
static int a_accept(lxp_socket_t listener, lxp_socket_t *out, uint64_t timeout_ns)
{
	struct lxp_socket *listen_socket = slot_lookup(listener);
	if (!listen_socket || !out)
		return OVE_ERR_INVALID_PARAM;
	struct lxp_socket *s = slot_alloc();
	if (!s)
		return LXP_ERR_NO_MEMORY;
	int r = ove_socket_accept(listen_socket->h, &s->h, &s->st, timeout_ns);
	if (r != OVE_OK) {
		s->used = 0; /* also the OVE_ERR_TIMEOUT (no pending connection) path */
		return r;
	}
	return slot_publish(s, out);
}
static void a_close(lxp_socket_t s)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket)
		return;
	slot_close(socket);
}
static int a_connect(lxp_socket_t s, const lxp_sockaddr_t *a, uint64_t t)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket || !a)
		return OVE_ERR_INVALID_PARAM;
	ove_sockaddr_t oa;
	to_ove(a, &oa);
	return ove_socket_connect(socket->h, &oa, t);
}
static int a_bind(lxp_socket_t s, const lxp_sockaddr_t *a)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket || !a)
		return OVE_ERR_INVALID_PARAM;
	ove_sockaddr_t oa;
	to_ove(a, &oa);
	return ove_socket_bind(socket->h, &oa);
}
static int a_listen(lxp_socket_t s, int backlog)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket)
		return OVE_ERR_INVALID_PARAM;
	return ove_socket_listen(socket->h, backlog);
}
static int a_send(lxp_socket_t s, const void *d, size_t n, size_t *sent)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket || (!d && n != 0))
		return OVE_ERR_INVALID_PARAM;
	return ove_socket_send(socket->h, d, n, sent);
}
static int a_recv(lxp_socket_t s, void *b, size_t n, size_t *got, uint64_t t)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket || (!b && n != 0))
		return OVE_ERR_INVALID_PARAM;
	return ove_socket_recv(socket->h, b, n, got, t);
}
static int a_sendto(lxp_socket_t s, const void *d, size_t n, size_t *sent,
		    const lxp_sockaddr_t *dst)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket || (!d && n != 0) || !dst)
		return OVE_ERR_INVALID_PARAM;
	ove_sockaddr_t oa;
	to_ove(dst, &oa);
	return ove_socket_sendto(socket->h, d, n, sent, &oa);
}
static int a_recvfrom(lxp_socket_t s, void *b, size_t n, size_t *got, lxp_sockaddr_t *src,
		      uint64_t t)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket || (!b && n != 0))
		return OVE_ERR_INVALID_PARAM;
	ove_sockaddr_t oa;
	int r = ove_socket_recvfrom(socket->h, b, n, got, &oa, t);
	if (r == OVE_OK && src)
		from_ove(&oa, src);
	return r;
}
static int a_set_nonblock(lxp_socket_t s, int nb)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket)
		return OVE_ERR_INVALID_PARAM;
	return ove_socket_set_nonblock(socket->h, nb);
}
static int a_poll(lxp_socket_t s, unsigned events, unsigned *revents, uint64_t t)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket)
		return OVE_ERR_INVALID_PARAM;
	return ove_socket_poll(socket->h, events, revents, t);
}
static int a_shutdown(lxp_socket_t s, int how)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket)
		return OVE_ERR_INVALID_PARAM;
	return ove_socket_shutdown(socket->h, how);
}
static int a_getsockname(lxp_socket_t s, lxp_sockaddr_t *a)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket || !a)
		return OVE_ERR_INVALID_PARAM;
	ove_sockaddr_t oa;
	int r = ove_socket_getsockname(socket->h, &oa);
	if (r == OVE_OK)
		from_ove(&oa, a);
	return r;
}
static int a_getpeername(lxp_socket_t s, lxp_sockaddr_t *a)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket || !a)
		return OVE_ERR_INVALID_PARAM;
	ove_sockaddr_t oa;
	int r = ove_socket_getpeername(socket->h, &oa);
	if (r == OVE_OK)
		from_ove(&oa, a);
	return r;
}
static int a_get_error(lxp_socket_t s)
{
	struct lxp_socket *socket = slot_lookup(s);
	if (!socket)
		return OVE_ERR_INVALID_PARAM;
	return ove_socket_get_error(socket->h);
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
	.run_begin = a_run_begin,
	.run_end = a_run_end,
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
#if defined(CONFIG_OVE_NET_RX_READY_NOTIFY)
	/* The ove_net backend publishes readiness without depending on LXP. */
	.capabilities = LXP_NET_CAP_SOCKET_READY_EVENT,
#endif
};

#endif /* CONFIG_OVE_LINUX_NET */
