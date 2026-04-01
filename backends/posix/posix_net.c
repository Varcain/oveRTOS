/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>

/* ---------- helpers ---------- */

static int errno_to_ove(int err)
{
	switch (err) {
	case ECONNREFUSED:  return OVE_ERR_NET_REFUSED;
	case ENETUNREACH:   /* fall through */
	case EHOSTUNREACH:  return OVE_ERR_NET_UNREACHABLE;
	case ETIMEDOUT:     return OVE_ERR_TIMEOUT;
	case EADDRINUSE:    return OVE_ERR_NET_ADDR_IN_USE;
	case ECONNRESET:    return OVE_ERR_NET_RESET;
	case ECONNABORTED:  return OVE_ERR_NET_RESET;
	case ENOTCONN:      return OVE_ERR_NET_CLOSED;
	case EPIPE:         return OVE_ERR_NET_CLOSED;
	default:            return OVE_ERR_NOT_SUPPORTED;
	}
}

static void sockaddr_to_posix(const ove_sockaddr_t *ove,
			      struct sockaddr_storage *ss, socklen_t *len)
{
	memset(ss, 0, sizeof(*ss));
	if (ove->family == OVE_AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *)ss;
		in->sin_family = AF_INET;
		in->sin_port = htons(ove->port);
		memcpy(&in->sin_addr, ove->addr, 4);
		*len = sizeof(*in);
	} else {
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(ove->port);
		memcpy(&in6->sin6_addr, ove->addr, 16);
		*len = sizeof(*in6);
	}
}

static void posix_to_sockaddr(const struct sockaddr_storage *ss,
			      ove_sockaddr_t *ove)
{
	memset(ove, 0, sizeof(*ove));
	if (ss->ss_family == AF_INET) {
		const struct sockaddr_in *in = (const struct sockaddr_in *)ss;
		ove->family = OVE_AF_INET;
		ove->port = ntohs(in->sin_port);
		memcpy(ove->addr, &in->sin_addr, 4);
	} else if (ss->ss_family == AF_INET6) {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)ss;
		ove->family = OVE_AF_INET6;
		ove->port = ntohs(in6->sin6_port);
		memcpy(ove->addr, &in6->sin6_addr, 16);
	}
}

static int af_to_posix(ove_af_t af)
{
	return (af == OVE_AF_INET6) ? AF_INET6 : AF_INET;
}

static int type_to_posix(ove_sock_type_t type)
{
	return (type == OVE_SOCK_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
}

/* ---------- Network interface ---------- */

int ove_netif_init(ove_netif_t *netif, ove_netif_storage_t *storage)
{
	int ret = ove_check_param(netif);
	if (ret) return ret;
	if (!storage) return OVE_ERR_INVALID_PARAM;
	struct ove_netif *n = (struct ove_netif *)storage;
	n->initialized = 1;
	*netif = n;
	return OVE_OK;
}

void ove_netif_deinit(ove_netif_t netif)
{
	if (netif) {
		netif->initialized = 0;
	}
}

int ove_netif_up(ove_netif_t netif, const ove_netif_config_t *cfg)
{
	(void)cfg;
	if (!netif) return OVE_ERR_INVALID_PARAM;
	/* POSIX: host OS manages networking — nothing to do. */
	return OVE_OK;
}

void ove_netif_down(ove_netif_t netif)
{
	(void)netif;
	/* POSIX: no-op. */
}

int ove_netif_get_addr(ove_netif_t netif, ove_sockaddr_t *ip,
		       ove_sockaddr_t *gateway, ove_sockaddr_t *netmask)
{
	if (!netif) return OVE_ERR_INVALID_PARAM;

	/* Use getifaddrs to find the first non-loopback IPv4 address. */
	struct ifaddrs *ifa_list, *ifa;
	if (getifaddrs(&ifa_list) != 0)
		return OVE_ERR_NOT_SUPPORTED;

	int found = 0;
	for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;

		struct sockaddr_in *sin;
		if (ip) {
			sin = (struct sockaddr_in *)ifa->ifa_addr;
			memset(ip, 0, sizeof(*ip));
			ip->family = OVE_AF_INET;
			memcpy(ip->addr, &sin->sin_addr.s_addr, 4);
		}
		if (netmask && ifa->ifa_netmask) {
			sin = (struct sockaddr_in *)ifa->ifa_netmask;
			memset(netmask, 0, sizeof(*netmask));
			netmask->family = OVE_AF_INET;
			memcpy(netmask->addr, &sin->sin_addr.s_addr, 4);
		}
		/* Gateway not available from getifaddrs — leave zeroed */
		if (gateway)
			memset(gateway, 0, sizeof(*gateway));
		found = 1;
		break;
	}

	freeifaddrs(ifa_list);
	return found ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_netif_create(ove_netif_t *netif)
{
	int ret = ove_check_param(netif);
	if (ret) return ret;
	struct ove_netif *n = OVE_BACKEND_MALLOC(sizeof(*n));
	if (!n) return OVE_ERR_NO_MEMORY;
	n->initialized = 1;
	*netif = n;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_netif_destroy(ove_netif_t netif)
{
	if (netif) {
		netif->initialized = 0;
		OVE_BACKEND_FREE(netif);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ---------- Socket ---------- */

int ove_socket_open(ove_socket_t *sock, ove_socket_storage_t *storage,
		    ove_af_t af, ove_sock_type_t type)
{
	int ret = ove_check_param(sock);
	if (ret) return ret;
	if (!storage) return OVE_ERR_INVALID_PARAM;
	struct ove_socket *s = (struct ove_socket *)storage;
	int fd = socket(af_to_posix(af), type_to_posix(type), 0);
	if (fd < 0) return errno_to_ove(errno);
	/* Enable SO_REUSEADDR for server sockets to avoid EADDRINUSE */
	int optval = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
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
	if (ret) return ret;
	struct ove_socket *s = OVE_BACKEND_MALLOC(sizeof(*s));
	if (!s) return OVE_ERR_NO_MEMORY;
	int fd = socket(af_to_posix(af), type_to_posix(type), 0);
	if (fd < 0) {
		OVE_BACKEND_FREE(s);
		return errno_to_ove(errno);
	}
	s->fd = fd;
	*sock = s;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_socket_destroy(ove_socket_t sock)
{
	if (sock) {
		if (sock->fd >= 0) close(sock->fd);
		OVE_BACKEND_FREE(sock);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr,
		       uint32_t timeout_ms)
{
	if (!sock || !addr) return OVE_ERR_INVALID_PARAM;

	struct sockaddr_storage ss;
	socklen_t slen;
	sockaddr_to_posix(addr, &ss, &slen);

	if (ove_timeout_is_forever(timeout_ms)) {
		if (connect(sock->fd, (struct sockaddr *)&ss, slen) < 0)
			return errno_to_ove(errno);
		return OVE_OK;
	}

	/* Non-blocking connect with timeout */
	int flags = fcntl(sock->fd, F_GETFL, 0);
	fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);

	int rc = connect(sock->fd, (struct sockaddr *)&ss, slen);
	if (rc < 0 && errno != EINPROGRESS) {
		fcntl(sock->fd, F_SETFL, flags);
		return errno_to_ove(errno);
	}

	if (rc == 0) {
		fcntl(sock->fd, F_SETFL, flags);
		return OVE_OK;
	}

	struct pollfd pfd = { .fd = sock->fd, .events = POLLOUT };
	int pr = poll(&pfd, 1, (int)timeout_ms);
	fcntl(sock->fd, F_SETFL, flags);

	if (pr == 0) return OVE_ERR_TIMEOUT;
	if (pr < 0) return errno_to_ove(errno);

	int so_err = 0;
	socklen_t elen = sizeof(so_err);
	getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &so_err, &elen);
	if (so_err) return errno_to_ove(so_err);

	return OVE_OK;
}

int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	if (!sock || !addr) return OVE_ERR_INVALID_PARAM;
	struct sockaddr_storage ss;
	socklen_t slen;
	sockaddr_to_posix(addr, &ss, &slen);
	if (bind(sock->fd, (struct sockaddr *)&ss, slen) < 0)
		return errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_listen(ove_socket_t sock, int backlog)
{
	if (!sock) return OVE_ERR_INVALID_PARAM;
	if (listen(sock->fd, backlog) < 0)
		return errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_accept(ove_socket_t sock, ove_socket_t *client,
		      ove_socket_storage_t *client_storage,
		      uint32_t timeout_ms)
{
	if (!sock || !client || !client_storage)
		return OVE_ERR_INVALID_PARAM;

	if (!ove_timeout_is_forever(timeout_ms)) {
		struct pollfd pfd = { .fd = sock->fd, .events = POLLIN };
		int pr = poll(&pfd, 1, (int)timeout_ms);
		if (pr == 0) return OVE_ERR_TIMEOUT;
		if (pr < 0) return errno_to_ove(errno);
	}

	int fd = accept(sock->fd, NULL, NULL);
	if (fd < 0) return errno_to_ove(errno);

	struct ove_socket *cs = (struct ove_socket *)client_storage;
	cs->fd = fd;
	*client = cs;
	return OVE_OK;
}

int ove_socket_send(ove_socket_t sock, const void *data, size_t len,
		    size_t *sent)
{
	if (!sock || !data) return OVE_ERR_INVALID_PARAM;
	ssize_t n = send(sock->fd, data, len, MSG_NOSIGNAL);
	if (n < 0) return errno_to_ove(errno);
	if (sent) *sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *buf, size_t len,
		    size_t *received, uint32_t timeout_ms)
{
	if (!sock || !buf) return OVE_ERR_INVALID_PARAM;

	if (!ove_timeout_is_forever(timeout_ms)) {
		struct pollfd pfd = { .fd = sock->fd, .events = POLLIN };
		int pr = poll(&pfd, 1, (int)timeout_ms);
		if (pr == 0) return OVE_ERR_TIMEOUT;
		if (pr < 0) return errno_to_ove(errno);
	}

	ssize_t n = recv(sock->fd, buf, len, 0);
	if (n < 0) return errno_to_ove(errno);
	if (n == 0) return OVE_ERR_NET_CLOSED;
	if (received) *received = (size_t)n;
	return OVE_OK;
}

int ove_socket_sendto(ove_socket_t sock, const void *data, size_t len,
		      size_t *sent, const ove_sockaddr_t *dest)
{
	if (!sock || !data || !dest) return OVE_ERR_INVALID_PARAM;
	struct sockaddr_storage ss;
	socklen_t slen;
	sockaddr_to_posix(dest, &ss, &slen);
	ssize_t n = sendto(sock->fd, data, len, MSG_NOSIGNAL,
			   (struct sockaddr *)&ss, slen);
	if (n < 0) return errno_to_ove(errno);
	if (sent) *sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len,
			size_t *received, ove_sockaddr_t *src,
			uint32_t timeout_ms)
{
	if (!sock || !buf) return OVE_ERR_INVALID_PARAM;

	if (!ove_timeout_is_forever(timeout_ms)) {
		struct pollfd pfd = { .fd = sock->fd, .events = POLLIN };
		int pr = poll(&pfd, 1, (int)timeout_ms);
		if (pr == 0) return OVE_ERR_TIMEOUT;
		if (pr < 0) return errno_to_ove(errno);
	}

	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	ssize_t n = recvfrom(sock->fd, buf, len, 0,
			     (struct sockaddr *)&ss, &slen);
	if (n < 0) return errno_to_ove(errno);
	if (n == 0) return OVE_ERR_NET_CLOSED;
	if (received) *received = (size_t)n;
	if (src) posix_to_sockaddr(&ss, src);
	return OVE_OK;
}

/* ---------- DNS ---------- */

int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr,
		    uint32_t timeout_ms)
{
	(void)timeout_ms; /* POSIX getaddrinfo has no timeout knob */
	if (!hostname || !addr) return OVE_ERR_INVALID_PARAM;

	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	int rc = getaddrinfo(hostname, NULL, &hints, &res);
	if (rc != 0) return OVE_ERR_NET_DNS_FAIL;

	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	struct sockaddr_in *in = (struct sockaddr_in *)res->ai_addr;
	memcpy(addr->addr, &in->sin_addr, 4);
	freeaddrinfo(res);
	return OVE_OK;
}

/* ---------- Address helpers ---------- */

void ove_sockaddr_ipv4(ove_sockaddr_t *addr, uint8_t a, uint8_t b,
		       uint8_t c, uint8_t d, uint16_t port)
{
	if (!addr) return;
	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	addr->port = port;
	addr->addr[0] = a;
	addr->addr[1] = b;
	addr->addr[2] = c;
	addr->addr[3] = d;
}
