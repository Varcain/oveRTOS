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
#include "netutils/netlib.h"
#endif

/* ---------- helpers ---------- */

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
	int ret = ove_check_param(sock);
	if (ret)
		return ret;
	if (!storage)
		return OVE_ERR_INVALID_PARAM;
	(void)af;
	struct ove_socket *s = (struct ove_socket *)storage;
	int stype = (type == OVE_SOCK_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
	int fd = socket(AF_INET, stype, 0);
	if (fd < 0)
		return errno_to_ove(errno);
	s->fd = fd;
	*sock = s;
	return OVE_OK;
}

void ove_socket_close(ove_socket_t sock)
{
	if (sock && sock->fd >= 0) {
		close(sock->fd);
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
	int fd = socket(AF_INET, stype, 0);
	if (fd < 0) {
		OVE_BACKEND_FREE(s);
		return errno_to_ove(errno);
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
			close(sock->fd);
		OVE_BACKEND_FREE(sock);
	}
}
#endif

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint32_t timeout_ms)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;

	struct sockaddr_in sin;
	sockaddr_to_nuttx(addr, &sin);

	if (ove_timeout_is_forever(timeout_ms)) {
		if (connect(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
			return errno_to_ove(errno);
		return OVE_OK;
	}

	/* Non-blocking connect with timeout */
	int flags = fcntl(sock->fd, F_GETFL, 0);
	fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);

	int rc = connect(sock->fd, (struct sockaddr *)&sin, sizeof(sin));
	if (rc < 0 && errno != EINPROGRESS) {
		fcntl(sock->fd, F_SETFL, flags);
		return errno_to_ove(errno);
	}
	if (rc == 0) {
		fcntl(sock->fd, F_SETFL, flags);
		return OVE_OK;
	}

	struct pollfd pfd = {.fd = sock->fd, .events = POLLOUT};
	int pr = poll(&pfd, 1, (int)timeout_ms);
	fcntl(sock->fd, F_SETFL, flags);

	if (pr == 0)
		return OVE_ERR_TIMEOUT;
	if (pr < 0)
		return errno_to_ove(errno);

	int so_err = 0;
	socklen_t elen = sizeof(so_err);
	getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &so_err, &elen);
	if (so_err)
		return errno_to_ove(so_err);

	return OVE_OK;
}

int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	sockaddr_to_nuttx(addr, &sin);
	if (bind(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		return errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_listen(ove_socket_t sock, int backlog)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	if (listen(sock->fd, backlog) < 0)
		return errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_accept(ove_socket_t sock, ove_socket_t *client, ove_socket_storage_t *client_storage,
		      uint32_t timeout_ms)
{
	if (!sock || !client || !client_storage)
		return OVE_ERR_INVALID_PARAM;

	if (!ove_timeout_is_forever(timeout_ms)) {
		struct pollfd pfd = {.fd = sock->fd, .events = POLLIN};
		int pr = poll(&pfd, 1, (int)timeout_ms);
		if (pr == 0)
			return OVE_ERR_TIMEOUT;
		if (pr < 0)
			return errno_to_ove(errno);
	}

	int fd = accept(sock->fd, NULL, NULL);
	if (fd < 0)
		return errno_to_ove(errno);

	struct ove_socket *cs = (struct ove_socket *)client_storage;
	cs->fd = fd;
	*client = cs;
	return OVE_OK;
}

int ove_socket_send(ove_socket_t sock, const void *data, size_t len, size_t *sent)
{
	if (!sock || !data)
		return OVE_ERR_INVALID_PARAM;
	ssize_t n = send(sock->fd, data, len, 0);
	if (n < 0)
		return errno_to_ove(errno);
	if (sent)
		*sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received, uint32_t timeout_ms)
{
	if (!sock || !buf)
		return OVE_ERR_INVALID_PARAM;

	if (!ove_timeout_is_forever(timeout_ms)) {
		struct pollfd pfd = {.fd = sock->fd, .events = POLLIN};
		int pr = poll(&pfd, 1, (int)timeout_ms);
		if (pr == 0)
			return OVE_ERR_TIMEOUT;
		if (pr < 0)
			return errno_to_ove(errno);
	}

	ssize_t n = recv(sock->fd, buf, len, 0);
	if (n < 0)
		return errno_to_ove(errno);
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
	ssize_t n = sendto(sock->fd, data, len, 0, (struct sockaddr *)&sin, sizeof(sin));
	if (n < 0)
		return errno_to_ove(errno);
	if (sent)
		*sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len, size_t *received,
			ove_sockaddr_t *src, uint32_t timeout_ms)
{
	if (!sock || !buf)
		return OVE_ERR_INVALID_PARAM;

	if (!ove_timeout_is_forever(timeout_ms)) {
		struct pollfd pfd = {.fd = sock->fd, .events = POLLIN};
		int pr = poll(&pfd, 1, (int)timeout_ms);
		if (pr == 0)
			return OVE_ERR_TIMEOUT;
		if (pr < 0)
			return errno_to_ove(errno);
	}

	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);
	ssize_t n = recvfrom(sock->fd, buf, len, 0, (struct sockaddr *)&sin, &slen);
	if (n < 0)
		return errno_to_ove(errno);
	if (n == 0)
		return OVE_ERR_NET_CLOSED;
	if (received)
		*received = (size_t)n;
	if (src)
		nuttx_to_sockaddr(&sin, src);
	return OVE_OK;
}

/* ---------- DNS ---------- */

int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint32_t timeout_ms)
{
	(void)timeout_ms;
	if (!hostname || !addr)
		return OVE_ERR_INVALID_PARAM;

	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	int rc = getaddrinfo(hostname, NULL, &hints, &res);
	if (rc != 0)
		return OVE_ERR_NET_DNS_FAIL;

	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
	memcpy(addr->addr, &sin->sin_addr, 4);
	freeaddrinfo(res);
	return OVE_OK;
}

/* ---------- Address helpers ---------- */

void ove_sockaddr_ipv4(ove_sockaddr_t *addr, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
		       uint16_t port)
{
	if (!addr)
		return;
	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	addr->port = port;
	addr->addr[0] = a;
	addr->addr[1] = b;
	addr->addr[2] = c;
	addr->addr[3] = d;
}
