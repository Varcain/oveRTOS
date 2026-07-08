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
 * (proc->sock_wait) and the run-loop coordinator retries via ove_lnx_sock_retry —
 * the same park/retry the pipe and device layers use.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_NET)

#include "ove/linux/net.h"
#include "ove/net.h"

#include <string.h>

/* fd-slot kind for a socket fd (fds[].file_idx = open-pool index). Kept in step
 * with the FD_* enumeration in ove_linux_syscall.c (free/console/file/pipe/tmpfs/
 * proc/dev = 0..6). */
#ifndef OVE_LNX_FD_SOCKET
#define OVE_LNX_FD_SOCKET 7
#endif

#define OVE_LNX_NSOCK 16 /* max concurrent socket opens (pooled) */

/** Per-open socket state (the 4-field fd slot is too small). fork/dup share an
 *  open (refcounted); the last close closes the backing ove_socket. */
struct sock_open {
	uint8_t used;
	uint8_t refs;
	uint8_t connecting;  /* a non-blocking connect is in flight */
	uint16_t oflags;     /* guest fd status flags (O_NONBLOCK gates parking) */
	uintptr_t rx_src;    /* parked recvfrom: user sockaddr* to fill (0 => recv) */
	uintptr_t rx_srclen; /* parked recvfrom: user socklen_t* */
	ove_socket_storage_t st;
	ove_socket_t sock;
};

static struct sock_open g_sock[OVE_LNX_NSOCK];

static struct sock_open *open_slot(int oi)
{
	if (oi < 0 || oi >= OVE_LNX_NSOCK || !g_sock[oi].used)
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
static void linux_sin_to_ove(const ove_lnx_sockaddr_in *sin, ove_sockaddr_t *oa)
{
	memset(oa, 0, sizeof(*oa));
	oa->family = OVE_AF_INET;
	oa->port = bswap16(sin->sin_port);
	memcpy(oa->addr, &sin->sin_addr, 4);
}

static void ove_to_linux_sin(const ove_sockaddr_t *oa, ove_lnx_sockaddr_in *sin)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = OVE_LNX_AF_INET;
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
		return -OVE_LNX_ECONNREFUSED;
	case OVE_ERR_NET_UNREACHABLE:
		return -OVE_LNX_ENETUNREACH;
	case OVE_ERR_NET_ADDR_IN_USE:
		return -OVE_LNX_EADDRINUSE;
	case OVE_ERR_NET_RESET:
		return -OVE_LNX_ECONNRESET;
	case OVE_ERR_NET_CLOSED:
		return -OVE_LNX_EPIPE;
	case OVE_ERR_TIMEOUT:
		return -OVE_LNX_EAGAIN;
	case OVE_ERR_INVALID_PARAM:
		return -OVE_LNX_EINVAL;
	case OVE_ERR_NO_MEMORY:
		return -OVE_LNX_ENOMEM;
	case OVE_ERR_NET_DNS_FAIL:
	case OVE_ERR_NOT_SUPPORTED:
	default:
		return -OVE_LNX_EOPNOTSUPP;
	}
}

/* Copy an ove_sockaddr_t out to a guest (sockaddr*, socklen_t*) pair, honouring
 * the caller's buffer cap and writing back the untruncated size (Linux semantics). */
static long copy_sockaddr_out(ove_lnx_proc_t *p, void *uaddr, void *uaddrlen,
			      const ove_sockaddr_t *oa)
{
	if (!uaddr || !uaddrlen)
		return 0;
	if (!user_ok(p, uaddrlen, sizeof(uint32_t), 1))
		return -OVE_LNX_EFAULT;
	uint32_t cap = *(uint32_t *)uaddrlen;
	ove_lnx_sockaddr_in sin;
	ove_to_linux_sin(oa, &sin);
	uint32_t n = cap < sizeof(sin) ? cap : (uint32_t)sizeof(sin);
	if (n && !user_ok(p, uaddr, n, 1))
		return -OVE_LNX_EFAULT;
	memcpy(uaddr, &sin, n);
	*(uint32_t *)uaddrlen = (uint32_t)sizeof(sin);
	return 0;
}

/* ---- socket(2) + open-pool lifecycle --------------------------------------- */

long ove_lnx_sock_new(int domain, int type, int protocol)
{
	if (domain != OVE_LNX_AF_INET)
		return -OVE_LNX_EAFNOSUPPORT;
	int base = type & OVE_LNX_SOCK_TYPE_MASK;
	ove_sock_type_t ot;
	if (base == OVE_LNX_SOCK_STREAM)
		ot = OVE_SOCK_STREAM;
	else if (base == OVE_LNX_SOCK_DGRAM)
		ot = OVE_SOCK_DGRAM;
	else
		return -OVE_LNX_EPROTONOSUPPORT; /* SOCK_RAW (ping) lands in P3 */
	(void)protocol;				 /* default proto for STREAM/DGRAM */

	int oi = -1;
	for (int i = 0; i < OVE_LNX_NSOCK; i++)
		if (!g_sock[i].used) {
			oi = i;
			break;
		}
	if (oi < 0)
		return -OVE_LNX_EMFILE;

	struct sock_open *o = &g_sock[oi];
	memset(o, 0, sizeof(*o));
	int r = ove_socket_open(&o->sock, &o->st, OVE_AF_INET, ot);
	if (r != OVE_OK)
		return ove_to_lnx_errno(r);
	/* Drive blocking via the coordinator's park/retry: keep the backing socket
	 * non-blocking so every op returns at once (a 0 timeout is NOT uniformly
	 * non-blocking — some backends map it to SO_RCVTIMEO = block-forever). */
	ove_socket_set_nonblock(o->sock, 1);
	o->used = 1;
	o->refs = 1;
	if (type & OVE_LNX_SOCK_NONBLOCK)
		o->oflags |= OVE_LNX_O_NONBLOCK;
	return oi;
}

void ove_lnx_sock_get(int oi)
{
	struct sock_open *o = open_slot(oi);
	if (o && o->refs < 0xff)
		o->refs++;
}

void ove_lnx_sock_close(int oi)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return;
	if (o->refs > 1) {
		o->refs--;
		return;
	}
	ove_socket_close(o->sock);
	o->used = 0;
}

void ove_lnx_sock_setfl(int oi, int flags)
{
	struct sock_open *o = open_slot(oi);
	if (o)
		o->oflags = (uint16_t)flags;
}

int ove_lnx_sock_getfl(int oi)
{
	struct sock_open *o = open_slot(oi);
	return o ? o->oflags : 0;
}

/* ---- connect / send / recv (with deferred-block park) ---------------------- */

long ove_lnx_sock_connect(ove_lnx_proc_t *p, int oi, const void *uaddr, unsigned addrlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;

	/* Re-entrant probe of an in-flight connect (a non-blocking guest that calls
	 * connect() again to poll for completion): report via SO_ERROR, don't
	 * re-initiate. */
	if (o->connecting) {
		unsigned rev = 0;
		ove_socket_poll(o->sock, OVE_SOCK_POLLOUT, &rev, 0);
		if (!(rev & (OVE_SOCK_POLLOUT | OVE_SOCK_POLLERR | OVE_SOCK_POLLHUP))) {
			if (o->oflags & OVE_LNX_O_NONBLOCK)
				return -OVE_LNX_EALREADY;
			p->sock_wait = OVE_LNX_SOCKW_CONNECT;
			p->sock_oi = oi;
			return 0;
		}
		int se = ove_socket_get_error(o->sock);
		o->connecting = 0;
		return se == OVE_OK ? -OVE_LNX_EISCONN : ove_to_lnx_errno(se);
	}

	if (!uaddr || addrlen < sizeof(ove_lnx_sockaddr_in) ||
	    !user_ok(p, uaddr, sizeof(ove_lnx_sockaddr_in), 0))
		return -OVE_LNX_EFAULT;
	const ove_lnx_sockaddr_in *sin = (const ove_lnx_sockaddr_in *)uaddr;
	if (sin->sin_family != OVE_LNX_AF_INET)
		return -OVE_LNX_EAFNOSUPPORT;
	ove_sockaddr_t oa;
	linux_sin_to_ove(sin, &oa);

	/* A 0 timeout initiates the connect and probes readiness once. */
	int r = ove_socket_connect(o->sock, &oa, 0);
	if (r == OVE_OK)
		return 0;
	if (r == OVE_ERR_TIMEOUT) { /* connection in progress */
		o->connecting = 1;
		if (o->oflags & OVE_LNX_O_NONBLOCK)
			return -OVE_LNX_EINPROGRESS;
		p->sock_wait = OVE_LNX_SOCKW_CONNECT;
		p->sock_oi = oi;
		return 0; /* parked */
	}
	return ove_to_lnx_errno(r);
}

long ove_lnx_sock_send(ove_lnx_proc_t *p, int oi, const void *ubuf, size_t len, int flags,
		       const void *udest, unsigned destlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	if (len && (!ubuf || !user_ok(p, ubuf, len, 0)))
		return -OVE_LNX_EFAULT;

	size_t sent = 0;
	int r;
	if (udest) {
		if (destlen < sizeof(ove_lnx_sockaddr_in) ||
		    !user_ok(p, udest, sizeof(ove_lnx_sockaddr_in), 0))
			return -OVE_LNX_EFAULT;
		ove_sockaddr_t oa;
		linux_sin_to_ove((const ove_lnx_sockaddr_in *)udest, &oa);
		r = ove_socket_sendto(o->sock, ubuf, len, &sent, &oa);
	} else {
		r = ove_socket_send(o->sock, ubuf, len, &sent);
	}
	if (r == OVE_OK)
		return (long)sent;
	if (r == OVE_ERR_TIMEOUT) {
		/* A blocked stream send parks; a datagram sendto returns EAGAIN (the
		 * park would need to remember its dest — added when needed). */
		if (udest || (o->oflags & OVE_LNX_O_NONBLOCK) || (flags & OVE_LNX_MSG_DONTWAIT))
			return -OVE_LNX_EAGAIN;
		p->sock_wait = OVE_LNX_SOCKW_SEND;
		p->sock_oi = oi;
		p->sock_buf = (uintptr_t)ubuf;
		p->sock_len = len;
		return 0; /* parked */
	}
	return ove_to_lnx_errno(r);
}

long ove_lnx_sock_recv(ove_lnx_proc_t *p, int oi, void *ubuf, size_t len, int flags, void *usrc,
		       void *usrclen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	if (len && (!ubuf || !user_ok(p, ubuf, len, 1)))
		return -OVE_LNX_EFAULT;

	size_t got = 0;
	ove_sockaddr_t src;
	int r;
	if (usrc)
		r = ove_socket_recvfrom(o->sock, ubuf, len, &got, &src, OVE_WAIT_FOREVER);
	else
		r = ove_socket_recv(o->sock, ubuf, len, &got, OVE_WAIT_FOREVER);

	if (r == OVE_OK) {
		if (usrc)
			(void)copy_sockaddr_out(p, usrc, usrclen, &src);
		return (long)got;
	}
	if (r == OVE_ERR_NET_CLOSED)
		return 0; /* EOF: peer performed an orderly shutdown */
	if (r == OVE_ERR_TIMEOUT) {
		if ((o->oflags & OVE_LNX_O_NONBLOCK) || (flags & OVE_LNX_MSG_DONTWAIT))
			return -OVE_LNX_EAGAIN;
		p->sock_wait = OVE_LNX_SOCKW_RECV;
		p->sock_oi = oi;
		p->sock_buf = (uintptr_t)ubuf;
		p->sock_len = len;
		o->rx_src = (uintptr_t)usrc; /* non-zero => recvfrom on the retry */
		o->rx_srclen = (uintptr_t)usrclen;
		return 0; /* parked */
	}
	return ove_to_lnx_errno(r);
}

long ove_lnx_sock_shutdown(int oi, int how)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	int oh = (how == 0) ? OVE_SHUT_RD : (how == 1) ? OVE_SHUT_WR : OVE_SHUT_RDWR;
	return ove_to_lnx_errno(ove_socket_shutdown(o->sock, oh));
}

long ove_lnx_sock_getsockname(ove_lnx_proc_t *p, int oi, void *uaddr, void *uaddrlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	ove_sockaddr_t oa;
	int r = ove_socket_getsockname(o->sock, &oa);
	if (r != OVE_OK)
		return ove_to_lnx_errno(r);
	return copy_sockaddr_out(p, uaddr, uaddrlen, &oa);
}

long ove_lnx_sock_getpeername(ove_lnx_proc_t *p, int oi, void *uaddr, void *uaddrlen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	ove_sockaddr_t oa;
	int r = ove_socket_getpeername(o->sock, &oa);
	if (r != OVE_OK)
		return ove_to_lnx_errno(r);
	return copy_sockaddr_out(p, uaddr, uaddrlen, &oa);
}

long ove_lnx_sock_getsockopt(ove_lnx_proc_t *p, int oi, int level, int optname, void *uval,
			     void *ulen)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	if (!uval || !ulen || !user_ok(p, ulen, sizeof(uint32_t), 1))
		return -OVE_LNX_EFAULT;
	int val = 0;
	if (level == OVE_LNX_SOL_SOCKET && optname == OVE_LNX_SO_ERROR) {
		int se = ove_socket_get_error(o->sock);
		val = (int)(-ove_to_lnx_errno(se)); /* positive Linux errno, or 0 */
	}
	/* Other options report 0 (accept-and-report; real passthrough in P4). */
	uint32_t cap = *(uint32_t *)ulen;
	uint32_t n = cap < sizeof(int) ? cap : (uint32_t)sizeof(int);
	if (n && !user_ok(p, uval, n, 1))
		return -OVE_LNX_EFAULT;
	memcpy(uval, &val, n);
	*(uint32_t *)ulen = (uint32_t)sizeof(int);
	return 0;
}

long ove_lnx_sock_setsockopt(ove_lnx_proc_t *p, int oi, int level, int optname, const void *uval,
			     unsigned len)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	(void)level;
	(void)optname;
	if (len && (!uval || !user_ok(p, uval, len, 0)))
		return -OVE_LNX_EFAULT;
	/* Accept-and-ignore: the socket is driven non-blocking with coordinator
	 * park/retry, so SO_RCVTIMEO / SO_REUSEADDR / TCP_NODELAY are no-ops here
	 * (P4 adds real passthrough for the options busybox depends on). */
	return 0;
}

unsigned ove_lnx_sock_poll(int oi)
{
	struct sock_open *o = open_slot(oi);
	if (!o)
		return 0;
	unsigned rev = 0, out = 0;
	if (ove_socket_poll(o->sock, OVE_SOCK_POLLIN | OVE_SOCK_POLLOUT, &rev, 0) != OVE_OK)
		return OVE_LNX_POLLIN; /* surface the condition via a read */
	if (rev & (OVE_SOCK_POLLIN | OVE_SOCK_POLLERR | OVE_SOCK_POLLHUP))
		out |= OVE_LNX_POLLIN;
	if (rev & OVE_SOCK_POLLOUT)
		out |= OVE_LNX_POLLOUT;
	return out;
}

void ove_lnx_sock_fstat(int oi, uint32_t *mode, uint64_t *size)
{
	(void)oi;
	if (mode)
		*mode = OVE_LNX_S_IFSOCK | 0666u;
	if (size)
		*size = 0;
}

/* ---- coordinator: retry a parked socket op --------------------------------- */

long ove_lnx_sock_retry(ove_lnx_proc_t *p)
{
	/* A parked poll() waits on a whole fd set, not one open; the syscall TU owns the
	 * fd table + per-kind readiness probes, so re-scan there. */
	if (p->sock_wait == OVE_LNX_SOCKW_POLL)
		return ove_lnx_poll_retry(p);

	struct sock_open *o = open_slot(p->sock_oi);
	if (!o)
		return -OVE_LNX_EBADF;
	switch (p->sock_wait) {
	case OVE_LNX_SOCKW_CONNECT: {
		unsigned rev = 0;
		ove_socket_poll(o->sock, OVE_SOCK_POLLOUT, &rev, 0);
		if (!(rev & (OVE_SOCK_POLLOUT | OVE_SOCK_POLLERR | OVE_SOCK_POLLHUP)))
			return -OVE_LNX_EAGAIN; /* still connecting */
		int se = ove_socket_get_error(o->sock);
		o->connecting = 0;
		return se == OVE_OK ? 0 : ove_to_lnx_errno(se);
	}
	case OVE_LNX_SOCKW_SEND: {
		size_t sent = 0;
		int r = ove_socket_send(o->sock, (const void *)p->sock_buf, p->sock_len, &sent);
		if (r == OVE_OK)
			return (long)sent;
		if (r == OVE_ERR_TIMEOUT)
			return -OVE_LNX_EAGAIN;
		return ove_to_lnx_errno(r);
	}
	case OVE_LNX_SOCKW_RECV: {
		size_t got = 0;
		ove_sockaddr_t src;
		int r;
		if (o->rx_src)
			r = ove_socket_recvfrom(o->sock, (void *)p->sock_buf, p->sock_len, &got,
						&src, OVE_WAIT_FOREVER);
		else
			r = ove_socket_recv(o->sock, (void *)p->sock_buf, p->sock_len, &got,
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
			return -OVE_LNX_EAGAIN;
		return ove_to_lnx_errno(r);
	}
	default:
		return -OVE_LNX_EINVAL;
	}
}

/* ---- fork / exit fd lifecycle ---------------------------------------------- */

void ove_lnx_sock_fork_inherit(ove_lnx_proc_t *child)
{
	for (int fd = 0; fd < OVE_LNX_MAX_FDS; fd++)
		if (child->fds[fd].kind == OVE_LNX_FD_SOCKET)
			ove_lnx_sock_get(child->fds[fd].file_idx);
}

void ove_lnx_sock_proc_exit(ove_lnx_proc_t *p)
{
	for (int fd = 0; fd < OVE_LNX_MAX_FDS; fd++)
		if (p->fds[fd].kind == OVE_LNX_FD_SOCKET) {
			ove_lnx_sock_close(p->fds[fd].file_idx);
			p->fds[fd].kind = 0; /* FD_FREE */
		}
}

#endif /* CONFIG_OVE_LINUX_NET */
