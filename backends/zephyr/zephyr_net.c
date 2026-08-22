/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Zephyr networking backend.
 *
 * Wraps the Zephyr BSD socket API (zsock_socket, zsock_connect, etc.)
 * to implement the oveRTOS socket interface.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"

#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dns_resolve.h>
#include <errno.h>
#include <string.h>

/* ---------- helpers ---------- */

/* ns -> Zephyr struct zsock_timeval with fast path: <4.29 sec inputs
 * use 32-bit divides (single-cycle UDIV on Cortex-M), >4.29 sec fall
 * back to a 64-bit divide. */
static inline void ove_ns_to_zsock_timeval(uint64_t ns, struct zsock_timeval *tv)
{
	if (ns <= (uint64_t)UINT32_MAX) {
		uint32_t n = (uint32_t)ns;
		tv->tv_sec = (time_t)(n / 1000000000u);
		tv->tv_usec = (suseconds_t)((n % 1000000000u) / 1000u);
	} else {
		tv->tv_sec = (time_t)(ns / 1000000000ULL);
		tv->tv_usec = (suseconds_t)((ns % 1000000000ULL) / 1000ULL);
	}
}

static int zephyr_errno_to_ove(int err)
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
	/*
	 * Zephyr's native IPv4 bind path returns ENOENT when the requested
	 * unicast address is absent from every interface.
	 */
	case ENOENT:
		return OVE_ERR_NET_ADDR_NOT_AVAILABLE;
	case ECONNRESET:
		return OVE_ERR_NET_RESET;
	case ECONNABORTED:
		return OVE_ERR_NET_RESET;
	case EAGAIN: /* would-block on a non-blocking socket */
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
		return OVE_ERR_TIMEOUT;
	default:
		return OVE_ERR_NOT_SUPPORTED;
	}
}

static void sockaddr_to_zephyr(const ove_sockaddr_t *ove, struct sockaddr_in *sin)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = htons(ove->port);
	memcpy(&sin->sin_addr, ove->addr, 4);
}

static void zephyr_to_sockaddr(const struct sockaddr_in *sin, ove_sockaddr_t *ove)
{
	memset(ove, 0, sizeof(*ove));
	ove->family = OVE_AF_INET;
	ove->port = ntohs(sin->sin_port);
	memcpy(ove->addr, &sin->sin_addr, 4);
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

	struct net_if *iface = net_if_get_default();
	if (!iface)
		return OVE_ERR_NOT_SUPPORTED;

	if (!cfg->use_dhcp) {
		struct in_addr addr, mask, gw;

		memcpy(&addr.s_addr, cfg->static_ip.addr, 4);
		memcpy(&mask.s_addr, cfg->netmask.addr, 4);
		memcpy(&gw.s_addr, cfg->gateway.addr, 4);

		net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
		net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
		net_if_ipv4_set_gw(iface, &gw);
	}

	/* Configure DNS server */
	if (cfg->dns.addr[0] | cfg->dns.addr[1] | cfg->dns.addr[2] | cfg->dns.addr[3]) {
		struct sockaddr_in dns_sa;
		memset(&dns_sa, 0, sizeof(dns_sa));
		dns_sa.sin_family = AF_INET;
		memcpy(&dns_sa.sin_addr.s_addr, cfg->dns.addr, 4);

		static struct dns_resolve_context *ctx;
		ctx = dns_resolve_get_default();
		if (ctx) {
			static const char *dns_servers[2];
			static char dns_str[16];
			(void)snprintf(dns_str, sizeof(dns_str), "%u.%u.%u.%u", cfg->dns.addr[0],
				       cfg->dns.addr[1], cfg->dns.addr[2], cfg->dns.addr[3]);
			dns_servers[0] = dns_str;
			dns_servers[1] = NULL;
			dns_resolve_close(ctx);
			dns_resolve_init(ctx, dns_servers, NULL);
		}
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

	struct net_if *iface = net_if_get_default();
	if (!iface)
		return OVE_ERR_NOT_SUPPORTED;

	if (ip) {
		memset(ip, 0, sizeof(*ip));
		ip->family = OVE_AF_INET;
		/* Zephyr 4.4 changed `net_if_ipv4_get_global_addr` to return
		 * `struct net_in_addr *` directly (was `struct net_if_addr *`,
		 * which wrapped it). */
		struct net_in_addr *unicast =
			net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (unicast)
			memcpy(ip->addr, unicast, 4);
	}
	if (gateway) {
		memset(gateway, 0, sizeof(*gateway));
		gateway->family = OVE_AF_INET;
		/* Read the interface gateway set by net_if_ipv4_set_gw (in ove_netif_up /
		 * ove_netif_set_addr). net_if_ipv4_router_find_default() reads the ND/route
		 * router table, which set_gw does NOT populate — it read back 0.0.0.0 even
		 * though off-link routing (DNS/wget to the internet via the gw) worked. */
		struct net_in_addr gw = net_if_ipv4_get_gw(iface);
		memcpy(gateway->addr, &gw, 4);
	}
	if (netmask) {
		memset(netmask, 0, sizeof(*netmask));
		netmask->family = OVE_AF_INET;
		struct net_in_addr *unicast_nm =
			net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (unicast_nm) {
			struct net_in_addr nm = net_if_ipv4_get_netmask_by_addr(iface, unicast_nm);
			memcpy(netmask->addr, &nm, 4);
		}
	}

	return OVE_OK;
}

/* P2 interface config, backed by the real Zephyr net_if (SIOCSIFADDR/NETMASK, route add gw).
 * `ifconfig eth0 <ip>` replaces the interface's IPv4 address; a netmask/gateway are applied when
 * supplied. Mirrors the address plumbing in ove_netif_up(). */
int ove_netif_set_addr(ove_netif_t netif, const ove_sockaddr_t *ip, const ove_sockaddr_t *netmask,
		       const ove_sockaddr_t *gateway)
{
	if (!netif)
		return OVE_ERR_INVALID_PARAM;
	struct net_if *iface = net_if_get_default();
	if (!iface)
		return OVE_ERR_NOT_SUPPORTED;

	if (ip) {
		struct in_addr addr;
		memcpy(&addr.s_addr, ip->addr, 4);
		/* Best-effort address replacement: drop the current global address, then add the
		 * new one (net_if_ipv4_addr_add allows aliases, so a leftover old address would
		 * make net_if_ipv4_get_global_addr() ambiguous). NOTE: net_if_ipv4_addr_rm() is
		 * not 100% reliable here, so an immediate `ifconfig <ip>` readback occasionally
		 * still shows the previous address — a Zephyr address-table quirk; the change does
		 * take effect. The read paths (ifconfig/route display) are fully reliable. */
		struct net_in_addr *cur = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (cur) {
			struct in_addr old;
			memcpy(&old.s_addr, cur, 4);
			(void)net_if_ipv4_addr_rm(iface, &old);
		}
		(void)net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
		if (netmask) {
			struct in_addr mask;
			memcpy(&mask.s_addr, netmask->addr, 4);
			(void)net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
		}
	}
	if (gateway) {
		struct in_addr gw;
		memcpy(&gw.s_addr, gateway->addr, 4);
		net_if_ipv4_set_gw(iface, &gw);
	}
	return OVE_OK;
}
int ove_netif_set_up(ove_netif_t netif, int up)
{
	if (!netif)
		return OVE_ERR_INVALID_PARAM;
	struct net_if *iface = net_if_get_default();
	if (!iface)
		return OVE_ERR_NOT_SUPPORTED;
	/* net_if_up/down return -EALREADY when the interface is already in the requested
	 * state; `ifconfig <ip>` issues SIOCSIFFLAGS(IFF_UP) on an already-up interface, so
	 * treat "already there" as success rather than failing the ioctl with EINVAL. */
	int rc = up ? net_if_up(iface) : net_if_down(iface);
	return (rc == 0 || rc == -EALREADY) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}
int ove_netif_get_hwaddr(ove_netif_t netif, uint8_t mac[6])
{
	if (!netif)
		return OVE_ERR_INVALID_PARAM;
	struct net_if *iface = net_if_get_default();
	if (!iface)
		return OVE_ERR_NOT_SUPPORTED;
	struct net_linkaddr *ll = net_if_get_link_addr(iface);
	if (!ll || ll->len < 6)
		return OVE_ERR_NOT_SUPPORTED;
	memcpy(mac, ll->addr, 6);
	return OVE_OK;
}
int ove_netif_get_flags(ove_netif_t netif, unsigned *flags)
{
	if (!netif || !flags)
		return OVE_ERR_INVALID_PARAM;
	struct net_if *iface = net_if_get_default();
	if (!iface)
		return OVE_ERR_NOT_SUPPORTED;
	unsigned f = 0;
	if (net_if_flag_is_set(iface, NET_IF_UP))
		f |= OVE_NETIF_FLAG_UP;
	if (net_if_flag_is_set(iface, NET_IF_RUNNING))
		f |= OVE_NETIF_FLAG_RUNNING;
	/* Ethernet L2 is always broadcast + multicast capable; Zephyr has no per-net_if
	 * flag for it (unlike UP/RUNNING), so report them for the ETHERNET interface. */
	if (net_if_l2(iface) == &NET_L2_GET_NAME(ETHERNET))
		f |= OVE_NETIF_FLAG_BROADCAST | OVE_NETIF_FLAG_MULTICAST;
	*flags = f;
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
	(void)af; /* Zephyr: AF_INET */
	struct ove_socket *s = (struct ove_socket *)storage;
	/* SOCK_RAW (CONFIG_NET_SOCKETS_INET_RAW) carries raw-ICMP for ping: the guest supplies
	 * the ICMP message incl. its checksum. The STM32 ETH HW checksum offload is disabled
	 * (# CONFIG_ETH_STM32_HW_CHECKSUM), so Zephyr does not overwrite it — no FreeRTOS-style
	 * "zero the csum for the MAC" quirk is needed here. */
	int stype = (type == OVE_SOCK_RAW)     ? SOCK_RAW
		    : (type == OVE_SOCK_DGRAM) ? SOCK_DGRAM
					       : SOCK_STREAM;
	int fd = zsock_socket(AF_INET, stype, proto);
	if (fd < 0)
		return zephyr_errno_to_ove(errno);
	s->fd = fd;
	*sock = s;
	return OVE_OK;
}

void ove_socket_close(ove_socket_t sock)
{
	if (sock && sock->fd >= 0) {
		zsock_close(sock->fd);
		sock->fd = -1;
	}
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
	int fd = zsock_socket(AF_INET, stype, 0);
	if (fd < 0) {
		OVE_BACKEND_FREE(s);
		return zephyr_errno_to_ove(errno);
	}
	s->fd = fd;
	*sock = s;
	return OVE_OK;
}
#endif

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_socket_destroy(ove_socket_t sock)
{
	if (sock) {
		if (sock->fd >= 0)
			zsock_close(sock->fd);
		OVE_BACKEND_FREE(sock);
	}
}
#endif

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint64_t timeout_ns)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	sockaddr_to_zephyr(addr, &sin);

	if (ove_timeout_is_forever(timeout_ns)) {
		if (zsock_connect(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
			return zephyr_errno_to_ove(errno);
		return OVE_OK;
	}

	/* Bounded connect: go non-blocking, then poll() for writability (or the
	 * timeout) and read SO_ERROR for the result. */
	int flags = zsock_fcntl(sock->fd, ZVFS_F_GETFL, 0);
	zsock_fcntl(sock->fd, ZVFS_F_SETFL, flags | ZVFS_O_NONBLOCK);

	int result;
	if (zsock_connect(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
		result = OVE_OK; /* completed immediately (e.g. loopback) */
	} else if (errno != EINPROGRESS) {
		result = zephyr_errno_to_ove(errno);
	} else {
		struct zsock_pollfd pfd = {.fd = sock->fd, .events = ZSOCK_POLLOUT};
		int timeout_ms = (timeout_ns / 1000000ULL > (uint64_t)INT32_MAX)
					 ? INT32_MAX
					 : (int)(timeout_ns / 1000000ULL);
		int pr = zsock_poll(&pfd, 1, timeout_ms);
		if (pr == 0) {
			result = OVE_ERR_TIMEOUT;
		} else if (pr < 0) {
			result = zephyr_errno_to_ove(errno);
		} else {
			int soerr = 0;
			socklen_t sl = sizeof(soerr);
			zsock_getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
			result = (soerr == 0) ? OVE_OK : zephyr_errno_to_ove(soerr);
		}
	}

	zsock_fcntl(sock->fd, ZVFS_F_SETFL, flags); /* restore blocking mode */
	return result;
}

int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	sockaddr_to_zephyr(addr, &sin);
	if (zsock_bind(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		return zephyr_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_listen(ove_socket_t sock, int backlog)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	if (zsock_listen(sock->fd, backlog) < 0)
		return zephyr_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_accept(ove_socket_t sock, ove_socket_t *client, ove_socket_storage_t *client_storage,
		      uint64_t timeout_ns)
{
	if (!sock || !client || !client_storage)
		return OVE_ERR_INVALID_PARAM;

	/* Bound the accept wait via SO_RCVTIMEO. */
	if (!ove_timeout_is_forever(timeout_ns)) {
		struct zsock_timeval tv;
		ove_ns_to_zsock_timeval(timeout_ns, &tv);
		zsock_setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	int fd = zsock_accept(sock->fd, NULL, NULL);
	if (fd < 0) {
		if (errno == EAGAIN)
			return OVE_ERR_TIMEOUT;
		return zephyr_errno_to_ove(errno);
	}
	struct ove_socket *cs = (struct ove_socket *)client_storage;
	cs->fd = fd;
	*client = cs;
	return OVE_OK;
}

int ove_socket_send(ove_socket_t sock, const void *data, size_t len, size_t *sent)
{
	if (!sock || !data)
		return OVE_ERR_INVALID_PARAM;
	ssize_t n = zsock_send(sock->fd, data, len, 0);
	if (n < 0)
		return zephyr_errno_to_ove(errno);
	if (sent)
		*sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received, uint64_t timeout_ns)
{
	if (!sock || !buf)
		return OVE_ERR_INVALID_PARAM;
	if (!ove_timeout_is_forever(timeout_ns)) {
		struct zsock_timeval tv;
		ove_ns_to_zsock_timeval(timeout_ns, &tv);
		zsock_setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	ssize_t n = zsock_recv(sock->fd, buf, len, 0);
	if (n < 0) {
		if (errno == EAGAIN)
			return OVE_ERR_TIMEOUT;
		return zephyr_errno_to_ove(errno);
	}
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
	sockaddr_to_zephyr(dest, &sin);

	/* Zephyr AF_INET SOCK_RAW is IP_HDRINCL-style: its TX path (context_setup_raw_ip_packet)
	 * reads the IPv4 header straight from the caller's buffer. Linux raw ICMP (busybox ping)
	 * hands the kernel the ICMP message only and lets it prepend the IP header. Bridge the
	 * models: for a raw socket, build a minimal IPv4 header in front of the guest's payload
	 * (leaving the header checksum 0 — Zephyr recomputes it since the STM32 HW checksum
	 * offload is off) so `ping` produces well-formed datagrams. The RX side already returns
	 * the full IP datagram (CONFIG_NET_SOCKETS_INET_RAW), which is what ping expects. */
	int stype = 0;
	socklen_t olen = sizeof(stype);
	if (zsock_getsockopt(sock->fd, SOL_SOCKET, ZSOCK_SO_TYPE, &stype, &olen) == 0 &&
	    stype == SOCK_RAW) {
		if (len > 512)
			return OVE_ERR_INVALID_PARAM;
		int proto = IPPROTO_ICMP;
		socklen_t plen = sizeof(proto);
		(void)zsock_getsockopt(sock->fd, SOL_SOCKET, ZSOCK_SO_PROTOCOL, &proto, &plen);
		struct net_if *iface = net_if_get_default();
		struct net_in_addr *src =
			iface ? net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) : NULL;
		/* Static (not on the stack): raw sends are serviced one-at-a-time on the single
		 * coordinator thread, and the coordinator's stack budget is tight. */
		static uint8_t buf[20 + 512];
		uint16_t tot = (uint16_t)(20u + len);
		buf[0] = 0x45;			/* IPv4, IHL 5 */
		buf[1] = 0;			/* DSCP/ECN */
		buf[2] = (uint8_t)(tot >> 8);	/* total length */
		buf[3] = (uint8_t)(tot & 0xff);
		buf[4] = 0;			/* identification */
		buf[5] = 0;
		buf[6] = 0;			/* flags / fragment offset */
		buf[7] = 0;
		buf[8] = 64;			/* TTL */
		buf[9] = (uint8_t)proto;	/* protocol (ICMP for ping) */
		buf[10] = 0;			/* header checksum — filled by Zephyr */
		buf[11] = 0;
		if (src)
			memcpy(&buf[12], src, 4);
		else
			memset(&buf[12], 0, 4);
		memcpy(&buf[16], dest->addr, 4); /* destination */
		memcpy(&buf[20], data, len);
		ssize_t rn = zsock_sendto(sock->fd, buf, 20u + len, 0, (struct sockaddr *)&sin,
					  sizeof(sin));
		if (rn < 0)
			return zephyr_errno_to_ove(errno);
		if (sent)
			*sent = (rn >= 20) ? (size_t)(rn - 20) : 0; /* report ICMP bytes sent */
		return OVE_OK;
	}

	ssize_t n = zsock_sendto(sock->fd, data, len, 0, (struct sockaddr *)&sin, sizeof(sin));
	if (n < 0)
		return zephyr_errno_to_ove(errno);
	if (sent)
		*sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len, size_t *received,
			ove_sockaddr_t *src, uint64_t timeout_ns)
{
	if (!sock || !buf)
		return OVE_ERR_INVALID_PARAM;
	if (!ove_timeout_is_forever(timeout_ns)) {
		struct zsock_timeval tv;
		ove_ns_to_zsock_timeval(timeout_ns, &tv);
		zsock_setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);
	ssize_t n = zsock_recvfrom(sock->fd, buf, len, 0, (struct sockaddr *)&sin, &slen);
	if (n < 0) {
		if (errno == EAGAIN)
			return OVE_ERR_TIMEOUT;
		return zephyr_errno_to_ove(errno);
	}
	if (n == 0)
		return OVE_ERR_NET_CLOSED;
	if (received)
		*received = (size_t)n;
	if (src)
		zephyr_to_sockaddr(&sin, src);
	return OVE_OK;
}

/* ---------- Non-blocking readiness (drives the Linux-personality park/retry) ---------- */

int ove_socket_set_nonblock(ove_socket_t sock, int nonblock)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int flags = zsock_fcntl(sock->fd, ZVFS_F_GETFL, 0);
	if (flags < 0)
		return zephyr_errno_to_ove(errno);
	flags = nonblock ? (flags | ZVFS_O_NONBLOCK) : (flags & ~ZVFS_O_NONBLOCK);
	if (zsock_fcntl(sock->fd, ZVFS_F_SETFL, flags) < 0)
		return zephyr_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_poll(ove_socket_t sock, unsigned events, unsigned *revents, uint64_t timeout_ns)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	struct zsock_pollfd pfd = {.fd = sock->fd, .events = 0};
	if (events & OVE_SOCK_POLLIN)
		pfd.events |= ZSOCK_POLLIN;
	if (events & OVE_SOCK_POLLOUT)
		pfd.events |= ZSOCK_POLLOUT;
	int timeout_ms = ove_timeout_is_forever(timeout_ns) ? -1
			 : (timeout_ns / 1000000ULL > (uint64_t)INT32_MAX)
				 ? INT32_MAX
				 : (int)(timeout_ns / 1000000ULL);
	int pr = zsock_poll(&pfd, 1, timeout_ms);
	if (pr < 0)
		return zephyr_errno_to_ove(errno);
	unsigned re = 0;
	if (pfd.revents & ZSOCK_POLLIN)
		re |= OVE_SOCK_POLLIN;
	if (pfd.revents & ZSOCK_POLLOUT)
		re |= OVE_SOCK_POLLOUT;
	if (pfd.revents & ZSOCK_POLLERR)
		re |= OVE_SOCK_POLLERR;
	if (pfd.revents & ZSOCK_POLLHUP)
		re |= OVE_SOCK_POLLHUP;
	if (revents)
		*revents = re;
	return OVE_OK;
}

int ove_socket_shutdown(ove_socket_t sock, int how)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int lh = (how == OVE_SHUT_RD)	? ZSOCK_SHUT_RD
		 : (how == OVE_SHUT_WR) ? ZSOCK_SHUT_WR
					: ZSOCK_SHUT_RDWR;
	if (zsock_shutdown(sock->fd, lh) < 0)
		return zephyr_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_getsockname(ove_socket_t sock, ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	socklen_t sl = sizeof(sin);
	if (zsock_getsockname(sock->fd, (struct sockaddr *)&sin, &sl) < 0)
		return zephyr_errno_to_ove(errno);
	zephyr_to_sockaddr(&sin, addr);
	return OVE_OK;
}

int ove_socket_getpeername(ove_socket_t sock, ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	socklen_t sl = sizeof(sin);
	if (zsock_getpeername(sock->fd, (struct sockaddr *)&sin, &sl) < 0)
		return zephyr_errno_to_ove(errno);
	zephyr_to_sockaddr(&sin, addr);
	return OVE_OK;
}

int ove_socket_get_error(ove_socket_t sock)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int soerr = 0;
	socklen_t sl = sizeof(soerr);
	if (zsock_getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0)
		return zephyr_errno_to_ove(errno);
	return soerr ? zephyr_errno_to_ove(soerr) : OVE_OK;
}

/* ---------- DNS ---------- */

/* DNS with timeout via Zephyr dns_resolve API + semaphore */

/* DNS shared state is guarded by s_dns_mutex (one resolve in flight) plus a
 * per-call generation token passed through the resolver's user_data.  On
 * timeout we cancel the pending query (dns_cancel_addr_info) and bump the
 * generation, so a late callback whose token no longer matches is dropped —
 * it cannot corrupt the next resolve or leave a phantom sem token. */
static K_MUTEX_DEFINE(s_dns_mutex);
static K_SEM_DEFINE(s_dns_sem, 0, 1);
static struct sockaddr_in s_dns_result_addr;
static volatile int s_dns_done;
static volatile uintptr_t s_dns_gen;

static void dns_result_cb(enum dns_resolve_status status, struct dns_addrinfo *info,
			  void *user_data)
{
	/* Ignore a late completion whose call already timed out / was
	 * superseded — its generation no longer matches the active one. */
	if ((uintptr_t)user_data != s_dns_gen)
		return;
	if (status == DNS_EAI_INPROGRESS && info) {
		if (info->ai_family == AF_INET) {
			memcpy(&s_dns_result_addr, &info->ai_addr, sizeof(s_dns_result_addr));
			s_dns_done = 1;
		}
	} else if (status == DNS_EAI_ALLDONE) {
		k_sem_give(&s_dns_sem);
	} else {
		s_dns_done = -1;
		k_sem_give(&s_dns_sem);
	}
}

int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint64_t timeout_ns)
{
	if (!hostname || !addr)
		return OVE_ERR_INVALID_PARAM;
	if (timeout_ns == 0)
		timeout_ns = OVE_SEC(10);

	/* Serialize: the result/done/sem globals have a single owner per call. */
	k_mutex_lock(&s_dns_mutex, K_FOREVER);

	/* Unique non-zero generation for this call; the callback echoes it back. */
	uintptr_t gen = ++s_dns_gen;
	if (gen == 0)
		gen = ++s_dns_gen;
	s_dns_done = 0;
	k_sem_reset(&s_dns_sem);

	uint16_t dns_id = 0;
	/* Zephyr DNS API takes int32_t ms; clamp to that range. */
	int32_t timeout_ms_clamped = (timeout_ns / 1000000ULL > (uint64_t)INT32_MAX)
					     ? INT32_MAX
					     : (int32_t)(timeout_ns / 1000000ULL);
	int rc;
	int rc_dns = dns_resolve_name(dns_resolve_get_default(), hostname, DNS_QUERY_TYPE_A,
				      &dns_id, dns_result_cb, (void *)gen, timeout_ms_clamped);
	if (rc_dns < 0) {
		rc = OVE_ERR_NET_DNS_FAIL;
	} else if (k_sem_take(&s_dns_sem, K_NSEC(timeout_ns)) != 0) {
		/* Cancel the pending query and invalidate this call so any late
		 * callback is recognized as stale and dropped. */
		dns_cancel_addr_info(dns_id);
		s_dns_gen++;
		rc = OVE_ERR_TIMEOUT;
	} else if (s_dns_done != 1) {
		rc = OVE_ERR_NET_DNS_FAIL;
	} else {
		memset(addr, 0, sizeof(*addr));
		addr->family = OVE_AF_INET;
		memcpy(addr->addr, &s_dns_result_addr.sin_addr, 4);
		rc = OVE_OK;
	}

	k_mutex_unlock(&s_dns_mutex);
	return rc;
}
