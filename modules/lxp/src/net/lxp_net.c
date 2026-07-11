/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux-personality socket core: a pooled per-open socket table bridged to the
 * engine-neutral ove_net HAL (lwIP / NuttX net / Zephyr net), and the routing the
 * FD_SOCKET branches of the syscall handlers call into. It mirrors the /dev device
 * layer (linux/dev/ove_linux_dev.c): the fd's file_idx indexes a refcounted open
 * pool, fork/dup share an open, and the last close closes the socket.
 *
 * Blocking is deferred, never inline: the backing ove_socket is kept non-blocking,
 * so every op returns at once; a would-block (OVE_ERR_TIMEOUT) parks the caller
 * (proc->sock_wait) and the run-loop coordinator retries via lxp_sock_retry —
 * the same park/retry the pipe and device layers use.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_NET)

#include "lxp/lxp_net.h"
#include "lxp/lxp_net_ops.h"
#include "ove/net.h"

#include <string.h>

/* fd-slot kind for a socket fd (fds[].file_idx = open-pool index). Kept in step
 * with the FD_* enumeration in ove_linux_syscall.c (free/console/file/pipe/tmpfs/
 * proc/dev = 0..6). */
#ifndef LXP_FD_SOCKET
#define LXP_FD_SOCKET 7
#endif

/** Per-open socket state (the 4-field fd slot is too small). fork/dup share an
 *  open (refcounted); the last close closes the backing ove_socket. */
struct sock_open {
	uint8_t used;
	uint8_t refs;
	uint8_t connecting;  /* a non-blocking connect is in flight */
	uint16_t oflags;     /* guest fd status flags (O_NONBLOCK gates parking) */
	uintptr_t rx_src;    /* parked recvfrom: user sockaddr* to fill (0 => recv) */
	uintptr_t rx_srclen; /* parked recvfrom: user socklen_t* */
	lxp_socket_t sock; /* host-owned handle; the adapter owns the storage */
};

static struct sock_open g_sock[LXP_NSOCK];

static struct sock_open *open_slot(int oi)
{
	if (oi < 0 || oi >= LXP_NSOCK || !g_sock[oi].used)
		return NULL;
	return &g_sock[oi];
}

/* ---- byte-order + address / errno translation ------------------------------ */

static inline uint16_t bswap16(uint16_t v)
{
	return (uint16_t)((v >> 8) | (v << 8));
}

/* Guest sockaddr_in (sin_port/sin_addr network order) -> ove_sockaddr_t
 * (port host order, addr[] the raw network-order bytes). */
static void linux_sin_to_ove(const lxp_sockaddr_in *sin, ove_sockaddr_t *oa)
{
	memset(oa, 0, sizeof(*oa));
	oa->family = OVE_AF_INET;
	oa->port = bswap16(sin->sin_port);
	memcpy(oa->addr, &sin->sin_addr, 4);
}

static void ove_to_linux_sin(const ove_sockaddr_t *oa, lxp_sockaddr_in *sin)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = LXP_AF_INET;
	sin->sin_port = bswap16(oa->port);
	memcpy(&sin->sin_addr, oa->addr, 4);
}

/* ove_net error -> negated Linux errno. OVE_ERR_TIMEOUT is the "would block"
 * signal from a non-blocking op and is handled by the caller before this. */
static long ove_to_lnx_errno(int e)
{
	switch (e) {
	case OVE_OK:
		return 0;
	case OVE_ERR_NET_REFUSED:
		return -LXP_ECONNREFUSED;
	case OVE_ERR_NET_UNREACHABLE:
		return -LXP_ENETUNREACH;
	case OVE_ERR_NET_ADDR_IN_USE:
		return -LXP_EADDRINUSE;
	case OVE_ERR_NET_RESET:
		return -LXP_ECONNRESET;
	case OVE_ERR_NET_CLOSED:
		return -LXP_EPIPE;
	case OVE_ERR_TIMEOUT:
		return -LXP_EAGAIN;
	case OVE_ERR_INVALID_PARAM:
		return -LXP_EINVAL;
	case OVE_ERR_NO_MEMORY:
		return -LXP_ENOMEM;
	case OVE_ERR_NET_DNS_FAIL:
	case OVE_ERR_NOT_SUPPORTED:
	default:
		return -LXP_EOPNOTSUPP;
	}
}

/* Copy an ove_sockaddr_t out to a guest (sockaddr*, socklen_t*) pair, honouring
 * the caller's buffer cap and writing back the untruncated size (Linux semantics). */
static long copy_sockaddr_out(lxp_proc_t *p, void *uaddr, void *uaddrlen,
			      const ove_sockaddr_t *oa)
{
	if (!uaddr || !uaddrlen)
		return 0;
	if (!user_ok(p, uaddrlen, sizeof(uint32_t), 1))
		return -LXP_EFAULT;
	uint32_t cap = *(uint32_t *)uaddrlen;
	lxp_sockaddr_in sin;
	ove_to_linux_sin(oa, &sin);
	uint32_t n = cap < sizeof(sin) ? cap : (uint32_t)sizeof(sin);
	if (n && !user_ok(p, uaddr, n, 1))
		return -LXP_EFAULT;
	memcpy(uaddr, &sin, n);
	*(uint32_t *)uaddrlen = (uint32_t)sizeof(sin);
	return 0;
}

/* ---- socket(2) + open-pool lifecycle --------------------------------------- */

long lxp_sock_new(int domain, int type, int protocol)
{
	if (domain != LXP_AF_INET)
		return -LXP_EAFNOSUPPORT;
	int base = type & LXP_SOCK_TYPE_MASK;
	ove_sock_type_t ot;
	int proto = 0; /* default protocol for the type */
	if (base == LXP_SOCK_STREAM) {
		ot = OVE_SOCK_STREAM;
	} else if (base == LXP_SOCK_DGRAM) {
		ot = OVE_SOCK_DGRAM;
	} else if (base == LXP_SOCK_RAW) {
		ot = OVE_SOCK_RAW;
		proto = protocol; /* e.g. IPPROTO_ICMP (1) for busybox ping */
	} else {
		return -LXP_EPROTONOSUPPORT;
	}

	int oi = -1;
	for (int i = 0; i < LXP_NSOCK; i++)
		if (!g_sock[i].used) {
			oi = i;
			break;
		}
	if (oi < 0)
		return -LXP_EMFILE;

	struct sock_open *o = &g_sock[oi];
	memset(o, 0, sizeof(*o));
	int r = g_lxp_net_ops->sock_open(OVE_AF_INET, ot, proto, &o->sock);
	if (r != OVE_OK)
		return ove_to_lnx_errno(r);
	/* Drive blocking via the coordinator's park/retry: keep the backing socket
	 * non-blocking so every op returns at once (a 0 timeout is NOT uniformly
	 * non-blocking — some backends map it to SO_RCVTIMEO = block-forever). */
	g_lxp_net_ops->sock_set_nonblock(o->sock, 1);
	o->used = 1;
	o->refs = 1;
	if (type & LXP_SOCK_NONBLOCK)
		o->oflags |= LXP_O_NONBLOCK;
	return oi;
}

void lxp_sock_get(int oi)
{
	struct sock_open *o = open_slot(oi);
	if (o && o->refs < 0xff)
		o->refs++;
}

void lxp_sock_close(int oi)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return;
	if (o->refs > 1) {
		o->refs--;
		return;
	}
	g_lxp_net_ops->sock_close(o->sock);
	o->used = 0;
}

void lxp_sock_setfl(int oi, int flags)
{
	struct sock_open *o = open_slot(oi);
	if (o)
		o->oflags = (uint16_t)flags;
}

int lxp_sock_getfl(int oi)
{
	struct sock_open *o = open_slot(oi);
	/* A socket is bidirectional → O_RDWR. uClibc fdopen(fd,"r+") checks F_GETFL's
	 * access mode and returns EINVAL (which busybox wget reports as "out of memory")
	 * if it looks read-only — so the access bits must be present, not just oflags. */
	return o ? (LXP_O_RDWR | (int)o->oflags) : -LXP_EBADF;
}

/* ---- connect / send / recv (with deferred-block park) ---------------------- */

long lxp_sock_connect(lxp_proc_t *p, int oi, const void *uaddr, unsigned addrlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;

	/* Re-entrant probe of an in-flight connect (a non-blocking guest that calls
	 * connect() again to poll for completion): report via SO_ERROR, don't
	 * re-initiate. */
	if (o->connecting) {
		unsigned rev = 0;
		g_lxp_net_ops->sock_poll(o->sock, OVE_SOCK_POLLOUT, &rev, 0);
		if (!(rev & (OVE_SOCK_POLLOUT | OVE_SOCK_POLLERR | OVE_SOCK_POLLHUP))) {
			if (o->oflags & LXP_O_NONBLOCK)
				return -LXP_EALREADY;
			p->sock_wait = LXP_SOCKW_CONNECT;
			p->sock_oi = oi;
			return 0;
		}
		int se = g_lxp_net_ops->sock_get_error(o->sock);
		o->connecting = 0;
		return se == OVE_OK ? -LXP_EISCONN : ove_to_lnx_errno(se);
	}

	if (!uaddr || addrlen < sizeof(lxp_sockaddr_in) ||
	    !user_ok(p, uaddr, sizeof(lxp_sockaddr_in), 0))
		return -LXP_EFAULT;
	const lxp_sockaddr_in *sin = (const lxp_sockaddr_in *)uaddr;
	if (sin->sin_family != LXP_AF_INET)
		return -LXP_EAFNOSUPPORT;
	ove_sockaddr_t oa;
	linux_sin_to_ove(sin, &oa);

	/* A 0 timeout initiates the connect and probes readiness once. */
	int r = g_lxp_net_ops->sock_connect(o->sock, &oa, 0);
	if (r == OVE_OK)
		return 0;
	if (r == OVE_ERR_TIMEOUT) { /* connection in progress */
		o->connecting = 1;
		if (o->oflags & LXP_O_NONBLOCK)
			return -LXP_EINPROGRESS;
		p->sock_wait = LXP_SOCKW_CONNECT;
		p->sock_oi = oi;
		return 0; /* parked */
	}
	return ove_to_lnx_errno(r);
}

long lxp_sock_bind(lxp_proc_t *p, int oi, const void *uaddr, unsigned addrlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	if (!uaddr || addrlen < sizeof(lxp_sockaddr_in) ||
	    !user_ok(p, uaddr, sizeof(lxp_sockaddr_in), 0))
		return -LXP_EFAULT;
	const lxp_sockaddr_in *sin = (const lxp_sockaddr_in *)uaddr;
	if (sin->sin_family != LXP_AF_INET)
		return -LXP_EAFNOSUPPORT;
	ove_sockaddr_t oa;
	linux_sin_to_ove(sin, &oa);
	return ove_to_lnx_errno(g_lxp_net_ops->sock_bind(o->sock, &oa));
}

long lxp_sock_listen(int oi, int backlog)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	return ove_to_lnx_errno(g_lxp_net_ops->sock_listen(o->sock, backlog));
}

/* Accept one pending connection on listen slot @lo into a fresh pool slot + fd.
 * Returns the new guest fd, -EAGAIN if none is pending yet (the caller parks or
 * reports would-block), or a negative errno. Runs both from accept(2) and, on a
 * parked accept, from the coordinator's retry — so it owns the whole mint. */
static long do_accept(lxp_proc_t *p, struct sock_open *lo, void *uaddr, void *uaddrlen,
		      int flags)
{
	int ci = -1;
	for (int i = 0; i < LXP_NSOCK; i++)
		if (!g_sock[i].used) {
			ci = i;
			break;
		}
	if (ci < 0)
		return -LXP_EMFILE; /* socket pool full */
	struct sock_open *co = &g_sock[ci];
	memset(co, 0, sizeof(*co));
	int r = g_lxp_net_ops->sock_accept(lo->sock, &co->sock, OVE_WAIT_FOREVER);
	if (r == OVE_ERR_TIMEOUT)
		return -LXP_EAGAIN; /* no pending connection (non-blocking listen socket) */
	if (r != OVE_OK)
		return ove_to_lnx_errno(r);
	co->used = 1;
	co->refs = 1;
	g_lxp_net_ops->sock_set_nonblock(co->sock, 1); /* every op returns at once; the coordinator parks */
	if (flags & LXP_SOCK_NONBLOCK)
		co->oflags |= LXP_O_NONBLOCK;
	if (uaddr) {
		ove_sockaddr_t pa;
		if (g_lxp_net_ops->sock_getpeername(co->sock, &pa) == OVE_OK)
			(void)copy_sockaddr_out(p, uaddr, uaddrlen, &pa);
	}
	int fd = lxp_fd_install(p, LXP_FD_SOCKET, ci);
	if (fd < 0) {
		g_lxp_net_ops->sock_close(co->sock);
		co->used = 0;
		return -LXP_EMFILE;
	}
	return fd;
}

long lxp_sock_accept(lxp_proc_t *p, int oi, void *uaddr, void *uaddrlen, int flags)
{
	struct sock_open *lo = open_slot(oi);
	if (!lo)
		return -LXP_EBADF;
	if (uaddr && (!uaddrlen || !user_ok(p, uaddrlen, sizeof(uint32_t), 1)))
		return -LXP_EFAULT;
	long r = do_accept(p, lo, uaddr, uaddrlen, flags);
	if (r == -LXP_EAGAIN) {
		if (lo->oflags & LXP_O_NONBLOCK)
			return -LXP_EAGAIN;
		p->sock_wait = LXP_SOCKW_ACCEPT; /* park; the retry re-runs do_accept */
		p->sock_oi = oi;
		p->sock_buf = (uintptr_t)uaddr;
		p->sock_len = (size_t)(uintptr_t)uaddrlen;
		return 0; /* parked */
	}
	return r; /* new fd, or a negative errno */
}

long lxp_sock_send(lxp_proc_t *p, int oi, const void *ubuf, size_t len, int flags,
		       const void *udest, unsigned destlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	if (len && (!ubuf || !user_ok(p, ubuf, len, 0)))
		return -LXP_EFAULT;

	size_t sent = 0;
	int r;
	ove_sockaddr_t oa;
	if (udest) {
		if (destlen < sizeof(lxp_sockaddr_in) ||
		    !user_ok(p, udest, sizeof(lxp_sockaddr_in), 0))
			return -LXP_EFAULT;
		linux_sin_to_ove((const lxp_sockaddr_in *)udest, &oa);
	}
	/* The engine transport (lwIP copy) runs in the privileged coordinator, which reads this guest
	 * buffer from physical memory through an uncached view; flush the guest's dirty D-cache lines
	 * first so it does not copy stale bytes (a no-op except on the D-cache-on STM32F746). */
	lxp_cache_clean(ubuf, len);
	if (udest)
		r = g_lxp_net_ops->sock_sendto(o->sock, ubuf, len, &sent, &oa);
	else
		r = g_lxp_net_ops->sock_send(o->sock, ubuf, len, &sent);
	if (r == OVE_OK)
		return (long)sent;
	if (r == OVE_ERR_TIMEOUT) {
		/* A blocked stream send parks; a datagram sendto returns EAGAIN (the
		 * park would need to remember its dest — added when needed). */
		if (udest || (o->oflags & LXP_O_NONBLOCK) || (flags & LXP_MSG_DONTWAIT))
			return -LXP_EAGAIN;
		p->sock_wait = LXP_SOCKW_SEND;
		p->sock_oi = oi;
		p->sock_buf = (uintptr_t)ubuf;
		p->sock_len = len;
		return 0; /* parked */
	}
	return ove_to_lnx_errno(r);
}

long lxp_sock_recv(lxp_proc_t *p, int oi, void *ubuf, size_t len, int flags, void *usrc,
		       void *usrclen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	if (len && (!ubuf || !user_ok(p, ubuf, len, 1)))
		return -LXP_EFAULT;

	size_t got = 0;
	ove_sockaddr_t src;
	int r;
	if (usrc)
		r = g_lxp_net_ops->sock_recvfrom(o->sock, ubuf, len, &got, &src, OVE_WAIT_FOREVER);
	else
		r = g_lxp_net_ops->sock_recv(o->sock, ubuf, len, &got, OVE_WAIT_FOREVER);

	if (r == OVE_OK) {
		if (usrc)
			(void)copy_sockaddr_out(p, usrc, usrclen, &src);
		return (long)got;
	}
	if (r == OVE_ERR_NET_CLOSED)
		return 0; /* EOF: peer performed an orderly shutdown */
	if (r == OVE_ERR_TIMEOUT) {
		if ((o->oflags & LXP_O_NONBLOCK) || (flags & LXP_MSG_DONTWAIT))
			return -LXP_EAGAIN;
		p->sock_wait = LXP_SOCKW_RECV;
		p->sock_oi = oi;
		p->sock_buf = (uintptr_t)ubuf;
		p->sock_len = len;
		o->rx_src = (uintptr_t)usrc; /* non-zero => recvfrom on the retry */
		o->rx_srclen = (uintptr_t)usrclen;
		return 0; /* parked */
	}
	return ove_to_lnx_errno(r);
}

long lxp_sock_shutdown(int oi, int how)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	int oh = (how == 0) ? OVE_SHUT_RD : (how == 1) ? OVE_SHUT_WR : OVE_SHUT_RDWR;
	return ove_to_lnx_errno(g_lxp_net_ops->sock_shutdown(o->sock, oh));
}

long lxp_sock_getsockname(lxp_proc_t *p, int oi, void *uaddr, void *uaddrlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	ove_sockaddr_t oa;
	int r = g_lxp_net_ops->sock_getsockname(o->sock, &oa);
	if (r != OVE_OK)
		return ove_to_lnx_errno(r);
	return copy_sockaddr_out(p, uaddr, uaddrlen, &oa);
}

long lxp_sock_getpeername(lxp_proc_t *p, int oi, void *uaddr, void *uaddrlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	ove_sockaddr_t oa;
	int r = g_lxp_net_ops->sock_getpeername(o->sock, &oa);
	if (r != OVE_OK)
		return ove_to_lnx_errno(r);
	return copy_sockaddr_out(p, uaddr, uaddrlen, &oa);
}

long lxp_sock_getsockopt(lxp_proc_t *p, int oi, int level, int optname, void *uval,
			     void *ulen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	if (!uval || !ulen || !user_ok(p, ulen, sizeof(uint32_t), 1))
		return -LXP_EFAULT;
	int val = 0;
	if (level == LXP_SOL_SOCKET && optname == LXP_SO_ERROR) {
		int se = g_lxp_net_ops->sock_get_error(o->sock);
		val = (int)(-ove_to_lnx_errno(se)); /* positive Linux errno, or 0 */
	}
	/* Other options report 0 (accept-and-report; real passthrough in P4). */
	uint32_t cap = *(uint32_t *)ulen;
	uint32_t n = cap < sizeof(int) ? cap : (uint32_t)sizeof(int);
	if (n && !user_ok(p, uval, n, 1))
		return -LXP_EFAULT;
	memcpy(uval, &val, n);
	*(uint32_t *)ulen = (uint32_t)sizeof(int);
	return 0;
}

long lxp_sock_setsockopt(lxp_proc_t *p, int oi, int level, int optname, const void *uval,
			     unsigned len)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	(void)level;
	(void)optname;
	if (len && (!uval || !user_ok(p, uval, len, 0)))
		return -LXP_EFAULT;
	/* Accept-and-ignore: the socket is driven non-blocking with coordinator
	 * park/retry, so SO_RCVTIMEO / SO_REUSEADDR / TCP_NODELAY are no-ops here
	 * (P4 adds real passthrough for the options busybox depends on). */
	return 0;
}

unsigned lxp_sock_poll(int oi)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return 0;
	unsigned rev = 0, out = 0;
	if (g_lxp_net_ops->sock_poll(o->sock, OVE_SOCK_POLLIN | OVE_SOCK_POLLOUT, &rev, 0) != OVE_OK)
		return LXP_POLLIN; /* surface the condition via a read */
	if (rev & (OVE_SOCK_POLLIN | OVE_SOCK_POLLERR | OVE_SOCK_POLLHUP))
		out |= LXP_POLLIN;
	if (rev & OVE_SOCK_POLLOUT)
		out |= LXP_POLLOUT;
	return out;
}

void lxp_sock_fstat(int oi, uint32_t *mode, uint64_t *size)
{
	(void)oi;
	if (mode)
		*mode = LXP_S_IFSOCK | 0666u;
	if (size)
		*size = 0;
}

/* ---- interface config (ifconfig / route) ioctls ---------------------------- */

static ove_netif_t g_lnx_netif; /* the interface the SIOC* ioctls act on (one eth0) */

void lxp_sock_set_netif(void *netif_handle)
{
	g_lnx_netif = (ove_netif_t)netif_handle;
}

/* Snapshot the registered interface for the /proc/net/{dev,route} generators (which live
 * in the syscall TU). Any out param may be NULL. Returns 0, or -1 if no interface. */
int lxp_sock_ifsnapshot(uint8_t ip[4], uint8_t gw[4], uint8_t nm[4], uint8_t mac[6],
			    unsigned *flags)
{
	if (!g_lnx_netif)
		return -1;
	ove_sockaddr_t sip = {0}, sgw = {0}, snm = {0};
	g_lxp_net_ops->netif_get_addr(g_lnx_netif, &sip, &sgw, &snm);
	if (ip)
		memcpy(ip, sip.addr, 4);
	if (gw)
		memcpy(gw, sgw.addr, 4);
	if (nm)
		memcpy(nm, snm.addr, 4);
	if (mac)
		g_lxp_net_ops->netif_get_hwaddr(g_lnx_netif, mac);
	if (flags) {
		unsigned f = 0;
		g_lxp_net_ops->netif_get_flags(g_lnx_netif, &f);
		*flags = f;
	}
	return 0;
}

/* Map the ove_netif flag bitmask to the guest's IFF_* value. */
static int16_t iff_from_ove(unsigned f)
{
	int16_t v = 0;
	if (f & OVE_NETIF_FLAG_UP)
		v |= LXP_IFF_UP;
	if (f & OVE_NETIF_FLAG_BROADCAST)
		v |= LXP_IFF_BROADCAST;
	if (f & OVE_NETIF_FLAG_LOOPBACK)
		v |= LXP_IFF_LOOPBACK;
	if (f & OVE_NETIF_FLAG_RUNNING)
		v |= LXP_IFF_RUNNING;
	if (f & OVE_NETIF_FLAG_MULTICAST)
		v |= LXP_IFF_MULTICAST;
	return v;
}

/* SIOCGIFCONF: report the single interface (eth0) into the caller's ifreq[]. */
static long sock_ifconf(lxp_proc_t *p, unsigned long arg)
{
	if (!user_ok(p, (void *)arg, sizeof(lxp_ifconf), 1))
		return -LXP_EFAULT;
	lxp_ifconf *ifc = (lxp_ifconf *)arg;
	if (!ifc->ifc_buf || ifc->ifc_len < (int)sizeof(lxp_ifreq)) {
		ifc->ifc_len = sizeof(lxp_ifreq); /* the space one interface needs */
		return 0;
	}
	if (!user_ok(p, (void *)(uintptr_t)ifc->ifc_buf, sizeof(lxp_ifreq), 1))
		return -LXP_EFAULT;
	lxp_ifreq *r = (lxp_ifreq *)(uintptr_t)ifc->ifc_buf;
	memset(r, 0, sizeof(*r));
	r->ifr_name[0] = 'e';
	r->ifr_name[1] = 't';
	r->ifr_name[2] = 'h';
	r->ifr_name[3] = '0';
	ove_sockaddr_t ip = {0};
	if (g_lnx_netif && g_lxp_net_ops->netif_get_addr(g_lnx_netif, &ip, NULL, NULL) == OVE_OK) {
		r->ifr_ifru.ifru_addr.sin_family = LXP_AF_INET;
		memcpy(&r->ifr_ifru.ifru_addr.sin_addr, ip.addr, 4);
	}
	ifc->ifc_len = sizeof(lxp_ifreq);
	return 0;
}

/* SIOCADDRT / SIOCDELRT: the only route op we honour is setting/clearing the default
 * gateway (rt_dst == 0.0.0.0). Others are accepted as a no-op so `route` doesn't error. */
static long sock_route(lxp_proc_t *p, unsigned long req, unsigned long arg)
{
	if (!user_ok(p, (void *)arg, sizeof(lxp_rtentry), 0))
		return -LXP_EFAULT;
	const lxp_rtentry *rt = (const lxp_rtentry *)arg;
	if (!g_lnx_netif)
		return -LXP_ENODEV;
	int is_default = (rt->rt_dst.sin_addr == 0);
	if (is_default && (rt->rt_flags & LXP_RTF_GATEWAY)) {
		ove_sockaddr_t gw = {0};
		gw.family = OVE_AF_INET;
		if (req == LXP_SIOCADDRT)
			memcpy(gw.addr, &rt->rt_gateway.sin_addr, 4); /* set gw */
		/* SIOCDELRT leaves gw all-zero (clears it). */
		int r = g_lxp_net_ops->netif_set_addr(g_lnx_netif, NULL, NULL, &gw);
		return r == OVE_OK ? 0 : ove_to_lnx_errno(r);
	}
	return 0; /* non-default routes: accept, nothing to program on a one-hop link */
}

long lxp_sock_ioctl(lxp_proc_t *p, unsigned long req, unsigned long arg)
{
	if (req == LXP_SIOCGIFCONF)
		return sock_ifconf(p, arg);
	if (req == LXP_SIOCADDRT || req == LXP_SIOCDELRT)
		return sock_route(p, req, arg);

	/* All the SIOC*IF* ops take a struct ifreq*. */
	if (!user_ok(p, (void *)arg, sizeof(lxp_ifreq), 1))
		return -LXP_EFAULT;
	lxp_ifreq *ifr = (lxp_ifreq *)arg;
	ove_netif_t nif = g_lnx_netif;
	if (!nif)
		return -LXP_ENODEV;

	switch (req) {
	case LXP_SIOCGIFFLAGS: {
		unsigned f = 0;
		g_lxp_net_ops->netif_get_flags(nif, &f);
		ifr->ifr_ifru.ifru_flags = iff_from_ove(f);
		return 0;
	}
	case LXP_SIOCSIFFLAGS:
		return g_lxp_net_ops->netif_set_up(nif, (ifr->ifr_ifru.ifru_flags & LXP_IFF_UP) ? 1 : 0) ==
				       OVE_OK
			       ? 0
			       : -LXP_EINVAL;
	case LXP_SIOCGIFADDR:
	case LXP_SIOCGIFNETMASK:
	case LXP_SIOCGIFBRDADDR: {
		ove_sockaddr_t ip = {0}, gw = {0}, nm = {0};
		if (g_lxp_net_ops->netif_get_addr(nif, &ip, &gw, &nm) != OVE_OK)
			return -LXP_ENODEV;
		lxp_sockaddr_in *out = &ifr->ifr_ifru.ifru_addr;
		memset(out, 0, sizeof(*out));
		out->sin_family = LXP_AF_INET;
		if (req == LXP_SIOCGIFNETMASK) {
			memcpy(&out->sin_addr, nm.addr, 4);
		} else if (req == LXP_SIOCGIFBRDADDR) {
			uint8_t *b = (uint8_t *)&out->sin_addr;
			for (int i = 0; i < 4; i++)
				b[i] = (uint8_t)(ip.addr[i] | (uint8_t)~nm.addr[i]);
		} else {
			memcpy(&out->sin_addr, ip.addr, 4);
		}
		return 0;
	}
	case LXP_SIOCSIFADDR:
	case LXP_SIOCSIFNETMASK: {
		lxp_sockaddr_in *in = &ifr->ifr_ifru.ifru_addr;
		if (in->sin_family != LXP_AF_INET)
			return -LXP_EINVAL;
		ove_sockaddr_t sa = {0};
		sa.family = OVE_AF_INET;
		memcpy(sa.addr, &in->sin_addr, 4);
		int r = (req == LXP_SIOCSIFADDR) ? g_lxp_net_ops->netif_set_addr(nif, &sa, NULL, NULL)
						     : g_lxp_net_ops->netif_set_addr(nif, NULL, &sa, NULL);
		return r == OVE_OK ? 0 : ove_to_lnx_errno(r);
	}
	case LXP_SIOCGIFHWADDR: {
		uint8_t mac[6] = {0};
		g_lxp_net_ops->netif_get_hwaddr(nif, mac);
		/* ifr_hwaddr is a struct sockaddr: sa_family (ARPHRD_ETHER) then the 6-byte MAC. */
		memset(ifr->ifr_ifru.ifru_raw, 0, sizeof(ifr->ifr_ifru.ifru_raw));
		ifr->ifr_ifru.ifru_raw[0] = (uint8_t)LXP_ARPHRD_ETHER;
		memcpy(ifr->ifr_ifru.ifru_raw + 2, mac, 6);
		return 0;
	}
	case LXP_SIOCGIFINDEX:
		ifr->ifr_ifru.ifru_ivalue = 1;
		return 0;
	case LXP_SIOCGIFMTU:
		ifr->ifr_ifru.ifru_ivalue = 1500;
		return 0;
	default:
		return -LXP_EOPNOTSUPP;
	}
}

/* ---- coordinator: retry a parked socket op --------------------------------- */

long lxp_sock_retry(lxp_proc_t *p)
{
	/* A parked poll() waits on a whole fd set, not one open; the syscall TU owns the
	 * fd table + per-kind readiness probes, so re-scan there. */
	if (p->sock_wait == LXP_SOCKW_POLL)
		return lxp_poll_retry(p);

	struct sock_open *o = open_slot(p->sock_oi);
	if (!o)
		return -LXP_EBADF;
	switch (p->sock_wait) {
	case LXP_SOCKW_CONNECT: {
		unsigned rev = 0;
		g_lxp_net_ops->sock_poll(o->sock, OVE_SOCK_POLLOUT, &rev, 0);
		if (!(rev & (OVE_SOCK_POLLOUT | OVE_SOCK_POLLERR | OVE_SOCK_POLLHUP)))
			return -LXP_EAGAIN; /* still connecting */
		int se = g_lxp_net_ops->sock_get_error(o->sock);
		o->connecting = 0;
		return se == OVE_OK ? 0 : ove_to_lnx_errno(se);
	}
	case LXP_SOCKW_SEND: {
		size_t sent = 0;
		int r = g_lxp_net_ops->sock_send(o->sock, (const void *)p->sock_buf, p->sock_len, &sent);
		if (r == OVE_OK)
			return (long)sent;
		if (r == OVE_ERR_TIMEOUT)
			return -LXP_EAGAIN;
		return ove_to_lnx_errno(r);
	}
	case LXP_SOCKW_RECV: {
		size_t got = 0;
		ove_sockaddr_t src;
		int r;
		if (o->rx_src)
			r = g_lxp_net_ops->sock_recvfrom(o->sock, (void *)p->sock_buf, p->sock_len, &got,
						&src, OVE_WAIT_FOREVER);
		else
			r = g_lxp_net_ops->sock_recv(o->sock, (void *)p->sock_buf, p->sock_len, &got,
					    OVE_WAIT_FOREVER);
		if (r == OVE_OK) {
			if (o->rx_src)
				(void)copy_sockaddr_out(p, (void *)o->rx_src,
							(void *)o->rx_srclen, &src);
			return (long)got;
		}
		if (r == OVE_ERR_NET_CLOSED)
			return 0;
		if (r == OVE_ERR_TIMEOUT)
			return -LXP_EAGAIN;
		return ove_to_lnx_errno(r);
	}
	case LXP_SOCKW_ACCEPT:
		/* Re-run the mint: a new fd when a client is now pending, -EAGAIN to stay
		 * parked, or a negative errno. sock_buf/sock_len hold the user addr/len. */
		return do_accept(p, o, (void *)p->sock_buf, (void *)(uintptr_t)p->sock_len, 0);
	default:
		return -LXP_EINVAL;
	}
}

/* ---- fork / exit fd lifecycle ---------------------------------------------- */

void lxp_sock_fork_inherit(lxp_proc_t *child)
{
	for (int fd = 0; fd < LXP_MAX_FDS; fd++)
		if (child->fds[fd].kind == LXP_FD_SOCKET)
			lxp_sock_get(child->fds[fd].file_idx);
}

void lxp_sock_proc_exit(lxp_proc_t *p)
{
	for (int fd = 0; fd < LXP_MAX_FDS; fd++)
		if (p->fds[fd].kind == LXP_FD_SOCKET) {
			lxp_sock_close(p->fds[fd].file_idx);
			p->fds[fd].kind = 0; /* FD_FREE */
		}
}

#endif /* CONFIG_OVE_LINUX_NET */
