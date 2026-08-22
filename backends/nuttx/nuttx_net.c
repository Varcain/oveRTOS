/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * NuttX networking backend.
 *
 * NuttX provides standard POSIX sockets so the implementation is
 * nearly identical to the POSIX backend.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <nuttx/net/dns.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_OVE_NET
#include <net/if.h>
#include <nuttx/net/dns.h>
#include <nuttx/net/net.h>   /* psock_* API + struct socket: fd-table-independent sockets */
#include <nuttx/semaphore.h> /* nxsem for the one-shot poll probe */
#include <nuttx/fs/ioctl.h>  /* FIONBIO */
#include "netutils/netlib.h"
#endif

/* ---------- helpers ---------- */

/* poll() expects timeout in int milliseconds. Convert ns to ms with
 * round-up and saturate to INT_MAX (effectively wait-forever). */
#include <limits.h>
static inline int ns_to_poll_ms(uint64_t timeout_ns)
{
	uint64_t ms = (timeout_ns + 999999ULL) / 1000000ULL;
	return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
}

static int errno_to_ove(int err)
{
	switch (err) {
	case ECONNREFUSED:
		return OVE_ERR_NET_REFUSED;
	case ENETUNREACH: /* fall through */
	case EHOSTUNREACH:
		return OVE_ERR_NET_UNREACHABLE;
	case ETIMEDOUT:
		return OVE_ERR_TIMEOUT;
	case EADDRINUSE:
		return OVE_ERR_NET_ADDR_IN_USE;
	case EADDRNOTAVAIL:
		return OVE_ERR_NET_ADDR_NOT_AVAILABLE;
	case ECONNRESET:
		return OVE_ERR_NET_RESET;
	case ECONNABORTED:
		return OVE_ERR_NET_RESET;
	case ENOTCONN:
		return OVE_ERR_NET_CLOSED;
	case EPIPE:
		return OVE_ERR_NET_CLOSED;
	case ENOMEM:
	case ENFILE:
	case EMFILE:
		return OVE_ERR_NO_MEMORY;
	case EAGAIN: /* would-block on a non-blocking socket */
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
		return OVE_ERR_TIMEOUT;
	default:
		return OVE_ERR_NOT_SUPPORTED;
	}
}

static void sockaddr_to_nuttx(const ove_sockaddr_t *ove, struct sockaddr_in *sin)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = htons(ove->port);
	memcpy(&sin->sin_addr.s_addr, ove->addr, 4);
}

static void nuttx_to_sockaddr(const struct sockaddr_in *sin, ove_sockaddr_t *ove)
{
	memset(ove, 0, sizeof(*ove));
	ove->family = OVE_AF_INET;
	ove->port = ntohs(sin->sin_port);
	memcpy(ove->addr, &sin->sin_addr.s_addr, 4);
}

/* ---------- Network interface ---------- */

int ove_netif_init(ove_netif_t *netif, ove_netif_storage_t *storage)
{
	int ret = ove_check_param(netif);
	if (ret)
		return ret;
	if (!storage)
		return OVE_ERR_INVALID_PARAM;
	struct ove_netif *n = (struct ove_netif *)storage;
	n->initialized = 1;
	*netif = n;
	return OVE_OK;
}

void ove_netif_deinit(ove_netif_t netif)
{
	if (netif)
		netif->initialized = 0;
}

int ove_netif_up(ove_netif_t netif, const ove_netif_config_t *cfg)
{
	if (!netif)
		return OVE_ERR_INVALID_PARAM;
	if (!cfg)
		return OVE_OK;

	const char *ifname = "eth0";

	if (!cfg->use_dhcp) {
		/* Static IP configuration. The netlib_* helpers each open a
		 * SOCK_DGRAM AF_INET socket internally to issue an ioctl —
		 * surface non-zero returns to the caller so a failed bring-up
		 * doesn't masquerade as success with a 0.0.0.0 readback. */
		struct in_addr addr;

		memcpy(&addr.s_addr, cfg->static_ip.addr, 4);
		if (netlib_set_ipv4addr(ifname, &addr) < 0)
			return errno_to_ove(errno);

		memcpy(&addr.s_addr, cfg->netmask.addr, 4);
		if (netlib_set_ipv4netmask(ifname, &addr) < 0)
			return errno_to_ove(errno);

		memcpy(&addr.s_addr, cfg->gateway.addr, 4);
		if (netlib_set_dripv4addr(ifname, &addr) < 0)
			return errno_to_ove(errno);

		if (netlib_ifup(ifname) < 0)
			return errno_to_ove(errno);
	}

	/* Configure DNS server */
	if (cfg->dns.addr[0] | cfg->dns.addr[1] | cfg->dns.addr[2] | cfg->dns.addr[3]) {
		struct sockaddr_in dns_addr;
		memset(&dns_addr, 0, sizeof(dns_addr));
		dns_addr.sin_family = AF_INET;
		memcpy(&dns_addr.sin_addr.s_addr, cfg->dns.addr, 4);
		dns_add_nameserver((struct sockaddr *)&dns_addr, sizeof(dns_addr));
	}

	return OVE_OK;
}

void ove_netif_down(ove_netif_t netif)
{
	(void)netif;
}

int ove_netif_get_addr(ove_netif_t netif, ove_sockaddr_t *ip, ove_sockaddr_t *gateway,
		       ove_sockaddr_t *netmask)
{
	if (!netif)
		return OVE_ERR_INVALID_PARAM;

	const char *ifname = "eth0";
	struct in_addr addr;

	if (ip) {
		memset(ip, 0, sizeof(*ip));
		ip->family = OVE_AF_INET;
		if (netlib_get_ipv4addr(ifname, &addr) >= 0)
			memcpy(ip->addr, &addr.s_addr, 4);
	}
	if (gateway) {
		memset(gateway, 0, sizeof(*gateway));
		gateway->family = OVE_AF_INET;
		if (netlib_get_dripv4addr(ifname, &addr) >= 0)
			memcpy(gateway->addr, &addr.s_addr, 4);
	}
	if (netmask) {
		memset(netmask, 0, sizeof(*netmask));
		netmask->family = OVE_AF_INET;
		if (netlib_get_ipv4netmask(ifname, &addr) >= 0)
			memcpy(netmask->addr, &addr.s_addr, 4);
	}

	return OVE_OK;
}

/* P2 interface config. FreeRTOS/lwIP is the lead engine for runtime ifconfig; here we
 * expose a plausible read-only view + accept-and-ignore setters so the personality's
 * `ifconfig` runs. A real NuttX netlib-backed impl lands when P2 is verified here. */
int ove_netif_set_addr(ove_netif_t netif, const ove_sockaddr_t *ip, const ove_sockaddr_t *netmask,
		       const ove_sockaddr_t *gateway)
{
	(void)ip;
	(void)netmask;
	(void)gateway;
	return netif ? OVE_OK : OVE_ERR_INVALID_PARAM;
}
int ove_netif_set_up(ove_netif_t netif, int up)
{
	(void)up;
	return netif ? OVE_OK : OVE_ERR_INVALID_PARAM;
}
int ove_netif_get_hwaddr(ove_netif_t netif, uint8_t mac[6])
{
	static const uint8_t synth[6] = {0x02, 0x00, 0x00, 0xDE, 0xAD, 0x01};
	if (!netif)
		return OVE_ERR_INVALID_PARAM;
	memcpy(mac, synth, 6);
	return OVE_OK;
}
int ove_netif_get_flags(ove_netif_t netif, unsigned *flags)
{
	if (!netif || !flags)
		return OVE_ERR_INVALID_PARAM;
	*flags = OVE_NETIF_FLAG_UP | OVE_NETIF_FLAG_BROADCAST | OVE_NETIF_FLAG_RUNNING |
		 OVE_NETIF_FLAG_MULTICAST;
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_netif_create(ove_netif_t *netif)
{
	int ret = ove_check_param(netif);
	if (ret)
		return ret;
	struct ove_netif *n = OVE_BACKEND_MALLOC(sizeof(*n));
	if (!n)
		return OVE_ERR_NO_MEMORY;
	n->initialized = 1;
	*netif = n;
	return OVE_OK;
}
#endif

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_netif_destroy(ove_netif_t netif)
{
	if (netif) {
		netif->initialized = 0;
		OVE_BACKEND_FREE(netif);
	}
}
#endif

/* ---------- Socket ---------- */

/* We store a raw NuttX 'struct socket' in ove_socket and drive it with the psock_* API
 * rather than a POSIX fd. Reason: the Linux personality's guest programs open sockets
 * during a syscall dispatched in the SVCall handler (the guest task's context) — a POSIX
 * fd would land in that guest's PER-TASK fd table, invisible to the coordinator task that
 * must drive the socket's park/retry (and to any other task). A raw 'struct socket' is
 * fd-table-independent, so any task can operate on it; the guest keeps its own Linux-fd
 * namespace at the module (lxp) layer. psock_* return 0/positive on success, a NEGATED
 * errno on failure, and self-serialize on net_lock (no external locking needed). */
#define PSOCK(s) ((FAR struct socket *)((void *)(s)->_psock))
_Static_assert(sizeof(struct socket) <= sizeof(((struct ove_socket *)0)->_psock),
	       "struct socket does not fit ove_socket._psock");

static int psockerr(int r)
{
	return errno_to_ove(-r);
}

/* A socket whose s_conn is NULL has been closed (psock_close NULLs it) or was never
 * initialized. Operating on it would be a PRIVILEGED null-pointer dereference that
 * HardFaults the whole system (not just the guest — the MPU only sandboxes unprivileged
 * accesses). Guard every op that would touch the connection so a stale reference returns
 * "closed" instead. */
static inline int psock_ok(FAR struct socket *p)
{
	return (p && p->s_conn) ? 1 : 0;
}

static inline int in_handler(void)
{
	uint32_t r;
	__asm__ volatile("mrs %0, ipsr" : "=r"(r));
	return (r & 0x1ffu) != 0;
}

/* A psock_* op takes net_lock. In the SVCall handler, blocking on a CONTENDED net_lock
 * would trigger an illegal context switch INSIDE the exception (NuttX hands the note
 * driver a garbage tcb → BusFault at tcb->pid). So in the handler: try-lock; on contention
 * DEFER (return 0 → the caller returns OVE_ERR_TIMEOUT and the coordinator retries the op
 * in THREAD mode, where net_lock blocks safely). On success we HOLD net_lock across the op
 * (the op's own net_lock is recursive → no further block) and release via net_op_end().
 * In thread mode we take nothing here and let the op self-lock. */
static inline int net_op_begin(int *held)
{
	*held = 0;
	if (in_handler()) {
		if (net_trylock() < 0)
			return 0; /* contended in an exception → must defer */
		*held = 1;
	}
	return 1;
}
static inline void net_op_end(int held)
{
	if (held)
		net_unlock();
}

int ove_socket_open(ove_socket_t *sock, ove_socket_storage_t *storage, ove_af_t af,
		    ove_sock_type_t type)
{
	return ove_socket_open_ex(sock, storage, af, type, 0);
}

int ove_socket_open_ex(ove_socket_t *sock, ove_socket_storage_t *storage, ove_af_t af,
		       ove_sock_type_t type, int proto)
{
	int ret = ove_check_param(sock);
	if (ret)
		return ret;
	if (!storage)
		return OVE_ERR_INVALID_PARAM;
	(void)af;
	struct ove_socket *s = (struct ove_socket *)storage;
	int stype = (type == OVE_SOCK_DGRAM)  ? SOCK_DGRAM
		    : (type == OVE_SOCK_RAW)  ? SOCK_RAW /* needs CONFIG_NET_ICMP_SOCKET */
					      : SOCK_STREAM;
	s->connect_pending = 0;
	int r = psock_socket(AF_INET, stype, proto, PSOCK(s));
	if (r < 0)
		return psockerr(r);
	*sock = s;
	return OVE_OK;
}

void ove_socket_close(ove_socket_t sock)
{
	if (sock)
		psock_close(PSOCK(sock));
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_socket_create(ove_socket_t *sock, ove_af_t af, ove_sock_type_t type)
{
	int ret = ove_check_param(sock);
	if (ret)
		return ret;
	struct ove_socket *s = OVE_BACKEND_MALLOC(sizeof(*s));
	if (!s)
		return OVE_ERR_NO_MEMORY;
	(void)af;
	int stype = (type == OVE_SOCK_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
	int r = psock_socket(AF_INET, stype, 0, PSOCK(s));
	if (r < 0) {
		OVE_BACKEND_FREE(s);
		return psockerr(r);
	}
	*sock = s;
	return OVE_OK;
}
#endif

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_socket_destroy(ove_socket_t sock)
{
	if (sock) {
		psock_close(PSOCK(sock));
		OVE_BACKEND_FREE(sock);
	}
}
#endif

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint64_t timeout_ns)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	(void)timeout_ns;
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	if (in_handler()) {
		/* A TCP connect drives ARP + the SYN and, for a socket that is not yet writable,
		 * WAITS (net_lockedwait) for completion — a blocking wait that, taken in the SVCall
		 * exception, does an illegal context switch and corrupts the scheduler. Defer: stash
		 * the target and park (return TIMEOUT → the module sets connecting=1). ove_socket_poll,
		 * called from the coordinator THREAD to drive the connecting socket, fires the real
		 * psock_connect() there, where the wait is legal. */
		memcpy(sock->caddr, addr->addr, 4);
		sock->cport = addr->port;
		sock->connect_pending = 1;
		return OVE_ERR_TIMEOUT;
	}
	/* Thread context (the coordinator's own blocking connects: netfs / boot smoke). */
	struct sockaddr_in sin;
	sockaddr_to_nuttx(addr, &sin);
	int r = psock_connect(PSOCK(sock), (struct sockaddr *)&sin, sizeof(sin));
	if (r == 0)
		return OVE_OK;
	if (r == -EINPROGRESS || r == -EALREADY)
		return OVE_ERR_TIMEOUT;
	return psockerr(r);
}

int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	sockaddr_to_nuttx(addr, &sin);
	int r = psock_bind(PSOCK(sock), (struct sockaddr *)&sin, sizeof(sin));
	return r < 0 ? psockerr(r) : OVE_OK;
}

int ove_socket_listen(ove_socket_t sock, int backlog)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int r = psock_listen(PSOCK(sock), backlog);
	return r < 0 ? psockerr(r) : OVE_OK;
}

int ove_socket_accept(ove_socket_t sock, ove_socket_t *client, ove_socket_storage_t *client_storage,
		      uint64_t timeout_ns)
{
	if (!sock || !client || !client_storage)
		return OVE_ERR_INVALID_PARAM;
	(void)timeout_ns;
	struct ove_socket *cs = (struct ove_socket *)client_storage;
	/* SOCK_NONBLOCK: return -EAGAIN when no connection is pending (the module parks and
	 * retries), and mark the accepted socket non-blocking too. */
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int _held;
	if (!net_op_begin(&_held))
		return OVE_ERR_TIMEOUT;
	int r = psock_accept(PSOCK(sock), NULL, NULL, PSOCK(cs), SOCK_NONBLOCK);
	net_op_end(_held);
	if (r < 0)
		return r == -EAGAIN ? OVE_ERR_TIMEOUT : psockerr(r);
	*client = cs;
	return OVE_OK;
}

int ove_socket_send(ove_socket_t sock, const void *data, size_t len, size_t *sent)
{
	if (!sock || !data)
		return OVE_ERR_INVALID_PARAM;
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int _held;
	if (!net_op_begin(&_held))
		return OVE_ERR_TIMEOUT;
	ssize_t n = psock_send(PSOCK(sock), data, len, 0);
	net_op_end(_held);
	if (n < 0)
		return psockerr((int)n);
	if (sent)
		*sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received, uint64_t timeout_ns)
{
	if (!sock || !buf)
		return OVE_ERR_INVALID_PARAM;
	(void)timeout_ns;
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int _held;
	if (!net_op_begin(&_held))
		return OVE_ERR_TIMEOUT;
	ssize_t n = psock_recvfrom(PSOCK(sock), buf, len, 0, NULL, NULL);
	net_op_end(_held);
	if (n < 0)
		return psockerr((int)n);
	if (n == 0)
		return OVE_ERR_NET_CLOSED;
	if (received)
		*received = (size_t)n;
	return OVE_OK;
}

int ove_socket_sendto(ove_socket_t sock, const void *data, size_t len, size_t *sent,
		      const ove_sockaddr_t *dest)
{
	if (!sock || !data || !dest)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	sockaddr_to_nuttx(dest, &sin);
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int _held;
	if (!net_op_begin(&_held))
		return OVE_ERR_TIMEOUT;
	ssize_t n = psock_sendto(PSOCK(sock), data, len, 0, (struct sockaddr *)&sin, sizeof(sin));
	net_op_end(_held);
	if (n < 0)
		return psockerr((int)n);
	if (sent)
		*sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len, size_t *received,
			ove_sockaddr_t *src, uint64_t timeout_ns)
{
	if (!sock || !buf)
		return OVE_ERR_INVALID_PARAM;
	(void)timeout_ns;
	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int _held;
	if (!net_op_begin(&_held))
		return OVE_ERR_TIMEOUT;
	ssize_t n = psock_recvfrom(PSOCK(sock), buf, len, 0, (struct sockaddr *)&sin, &slen);
	net_op_end(_held);
	if (n < 0)
		return psockerr((int)n);
	if (n == 0)
		return OVE_ERR_NET_CLOSED;
	if (received)
		*received = (size_t)n;
	if (src)
		nuttx_to_sockaddr(&sin, src);
	return OVE_OK;
}

/* ---------- Non-blocking readiness (drives the Linux-personality park/retry) ---------- */

int ove_socket_set_nonblock(ove_socket_t sock, int nonblock)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int on = nonblock ? 1 : 0;
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int r = psock_ioctl(PSOCK(sock), FIONBIO, &on);
	return r < 0 ? psockerr(r) : OVE_OK;
}

int ove_socket_poll(ove_socket_t sock, unsigned events, unsigned *revents, uint64_t timeout_ns)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	(void)timeout_ns;
	if (!psock_ok(PSOCK(sock))) {
		if (revents)
			*revents = OVE_SOCK_POLLHUP;
		return OVE_OK;
	}
	/* Fire a deferred connect (stashed by ove_socket_connect in the handler) now that we run in
	 * the coordinator thread, where the connect's completion-wait is legal. Only in thread mode:
	 * the guest may also poll() the connecting fd from the handler, which must NOT initiate. */
	if (sock->connect_pending && !in_handler()) {
		sock->connect_pending = 0;
		struct sockaddr_in csin;
		memset(&csin, 0, sizeof(csin));
		csin.sin_family = AF_INET;
		memcpy(&csin.sin_addr.s_addr, sock->caddr, 4);
		csin.sin_port = htons(sock->cport);
		(void)psock_connect(PSOCK(sock), (struct sockaddr *)&csin, sizeof(csin));
		/* -EINPROGRESS expected; readiness is reported by the probe below (and subsequent
		 * polls), and the module reads SO_ERROR via ove_socket_get_error to finalize. */
	}
	/* One-shot readiness probe on the raw socket (no fd). CRITICAL: cb == NULL so that if
	 * the socket becomes ready between setup and teardown, poll_notify() sees a NULL cb and
	 * does NOT call back into this (soon-to-be-gone) stack frame — it only writes revents.
	 * psock_poll(setup=true) evaluates current readiness into fds.revents; tear it straight
	 * back down (only if setup succeeded — a failed setup leaves fds.priv NULL). No sem
	 * needed since we never wait. */
	struct pollfd fds;
	memset(&fds, 0, sizeof(fds));
	fds.fd = -1;
	if (events & OVE_SOCK_POLLIN)
		fds.events |= POLLIN;
	if (events & OVE_SOCK_POLLOUT)
		fds.events |= POLLOUT;
	int _held;
	if (!net_op_begin(&_held)) {
		if (revents)
			*revents = 0; /* contended in handler: report not-ready, coordinator re-polls */
		return OVE_OK;
	}
	int r = psock_poll(PSOCK(sock), &fds, true);
	unsigned re = (r < 0) ? (unsigned)POLLERR : (unsigned)fds.revents;
	if (r >= 0)
		psock_poll(PSOCK(sock), &fds, false);
	net_op_end(_held);

	unsigned out = 0;
	if (re & POLLIN)
		out |= OVE_SOCK_POLLIN;
	if (re & POLLOUT)
		out |= OVE_SOCK_POLLOUT;
	if (re & POLLERR)
		out |= OVE_SOCK_POLLERR;
	if (re & POLLHUP)
		out |= OVE_SOCK_POLLHUP;
	if (revents)
		*revents = out;
	return OVE_OK;
}

int ove_socket_shutdown(ove_socket_t sock, int how)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int lh = (how == OVE_SHUT_RD) ? SHUT_RD : (how == OVE_SHUT_WR) ? SHUT_WR : SHUT_RDWR;
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int r = psock_shutdown(PSOCK(sock), lh);
	return r < 0 ? psockerr(r) : OVE_OK;
}

int ove_socket_getsockname(ove_socket_t sock, ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	socklen_t sl = sizeof(sin);
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int r = psock_getsockname(PSOCK(sock), (struct sockaddr *)&sin, &sl);
	if (r < 0)
		return psockerr(r);
	nuttx_to_sockaddr(&sin, addr);
	return OVE_OK;
}

int ove_socket_getpeername(ove_socket_t sock, ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	socklen_t sl = sizeof(sin);
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int r = psock_getpeername(PSOCK(sock), (struct sockaddr *)&sin, &sl);
	if (r < 0)
		return psockerr(r);
	nuttx_to_sockaddr(&sin, addr);
	return OVE_OK;
}

int ove_socket_get_error(ove_socket_t sock)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int soerr = 0;
	socklen_t sl = sizeof(soerr);
	if (!psock_ok(PSOCK(sock)))
		return OVE_ERR_NET_CLOSED;
	int r = psock_getsockopt(PSOCK(sock), SOL_SOCKET, SO_ERROR, &soerr, &sl);
	if (r < 0)
		return psockerr(r);
	return soerr ? errno_to_ove(soerr) : OVE_OK;
}
