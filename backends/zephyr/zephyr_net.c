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

static int zephyr_errno_to_ove(int err)
{
	switch (err) {
	case ECONNREFUSED:  return OVE_ERR_NET_REFUSED;
	case ENETUNREACH:   /* fall through */
	case EHOSTUNREACH:  return OVE_ERR_NET_UNREACHABLE;
	case ETIMEDOUT:     return OVE_ERR_TIMEOUT;
	case EADDRINUSE:    return OVE_ERR_NET_ADDR_IN_USE;
	case ECONNRESET:    return OVE_ERR_NET_RESET;
	case ECONNABORTED:  return OVE_ERR_NET_RESET;
	default:            return OVE_ERR_NOT_SUPPORTED;
	}
}

static void sockaddr_to_zephyr(const ove_sockaddr_t *ove,
			       struct sockaddr_in *sin)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = htons(ove->port);
	memcpy(&sin->sin_addr, ove->addr, 4);
}

static void zephyr_to_sockaddr(const struct sockaddr_in *sin,
			       ove_sockaddr_t *ove)
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
	if (ret) return ret;
	if (!storage) return OVE_ERR_INVALID_PARAM;
	struct ove_netif *n = (struct ove_netif *)storage;
	n->initialized = 1;
	*netif = n;
	return OVE_OK;
}

void ove_netif_deinit(ove_netif_t netif)
{
	if (netif) netif->initialized = 0;
}

int ove_netif_up(ove_netif_t netif, const ove_netif_config_t *cfg)
{
	if (!netif) return OVE_ERR_INVALID_PARAM;
	if (!cfg) return OVE_OK;

	struct net_if *iface = net_if_get_default();
	if (!iface) return OVE_ERR_NOT_SUPPORTED;

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
	if (cfg->dns.addr[0] | cfg->dns.addr[1] |
	    cfg->dns.addr[2] | cfg->dns.addr[3]) {
		struct sockaddr_in dns_sa;
		memset(&dns_sa, 0, sizeof(dns_sa));
		dns_sa.sin_family = AF_INET;
		memcpy(&dns_sa.sin_addr.s_addr, cfg->dns.addr, 4);

		static struct dns_resolve_context *ctx;
		ctx = dns_resolve_get_default();
		if (ctx) {
			static const char *dns_servers[2];
			static char dns_str[16];
			snprintf(dns_str, sizeof(dns_str), "%u.%u.%u.%u",
				 cfg->dns.addr[0], cfg->dns.addr[1],
				 cfg->dns.addr[2], cfg->dns.addr[3]);
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

int ove_netif_get_addr(ove_netif_t netif, ove_sockaddr_t *ip,
		       ove_sockaddr_t *gateway, ove_sockaddr_t *netmask)
{
	if (!netif) return OVE_ERR_INVALID_PARAM;

	struct net_if *iface = net_if_get_default();
	if (!iface) return OVE_ERR_NOT_SUPPORTED;

	if (ip) {
		memset(ip, 0, sizeof(*ip));
		ip->family = OVE_AF_INET;
		struct net_if_addr *unicast =
			net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (unicast)
			memcpy(ip->addr, &unicast->address.in_addr, 4);
	}
	if (gateway) {
		memset(gateway, 0, sizeof(*gateway));
		gateway->family = OVE_AF_INET;
		struct net_if_router *router = net_if_ipv4_router_find_default(
			NULL, iface);
		if (router)
			memcpy(gateway->addr, &router->address.in_addr, 4);
	}
	if (netmask) {
		memset(netmask, 0, sizeof(*netmask));
		netmask->family = OVE_AF_INET;
		struct net_if_addr *unicast_nm =
			net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (unicast_nm) {
			struct net_in_addr nm = net_if_ipv4_get_netmask_by_addr(
				iface, &unicast_nm->address.in_addr);
			memcpy(netmask->addr, &nm, 4);
		}
	}

	return OVE_OK;
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

int ove_socket_open(ove_socket_t *sock, ove_socket_storage_t *storage,
		    ove_af_t af, ove_sock_type_t type)
{
	int ret = ove_check_param(sock);
	if (ret) return ret;
	if (!storage) return OVE_ERR_INVALID_PARAM;
	(void)af; /* Zephyr: AF_INET */
	struct ove_socket *s = (struct ove_socket *)storage;
	int stype = (type == OVE_SOCK_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
	int fd = zsock_socket(AF_INET, stype, 0);
	if (fd < 0) return zephyr_errno_to_ove(errno);
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
	if (ret) return ret;
	struct ove_socket *s = OVE_BACKEND_MALLOC(sizeof(*s));
	if (!s) return OVE_ERR_NO_MEMORY;
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
		if (sock->fd >= 0) zsock_close(sock->fd);
		OVE_BACKEND_FREE(sock);
	}
}
#endif

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr,
		       uint32_t timeout_ms)
{
	if (!sock || !addr) return OVE_ERR_INVALID_PARAM;
	(void)timeout_ms;
	struct sockaddr_in sin;
	sockaddr_to_zephyr(addr, &sin);
	if (zsock_connect(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		return zephyr_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	if (!sock || !addr) return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	sockaddr_to_zephyr(addr, &sin);
	if (zsock_bind(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		return zephyr_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_listen(ove_socket_t sock, int backlog)
{
	if (!sock) return OVE_ERR_INVALID_PARAM;
	if (zsock_listen(sock->fd, backlog) < 0)
		return zephyr_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_accept(ove_socket_t sock, ove_socket_t *client,
		      ove_socket_storage_t *client_storage,
		      uint32_t timeout_ms)
{
	if (!sock || !client || !client_storage)
		return OVE_ERR_INVALID_PARAM;
	(void)timeout_ms;
	int fd = zsock_accept(sock->fd, NULL, NULL);
	if (fd < 0) return zephyr_errno_to_ove(errno);
	struct ove_socket *cs = (struct ove_socket *)client_storage;
	cs->fd = fd;
	*client = cs;
	return OVE_OK;
}

int ove_socket_send(ove_socket_t sock, const void *data, size_t len,
		    size_t *sent)
{
	if (!sock || !data) return OVE_ERR_INVALID_PARAM;
	ssize_t n = zsock_send(sock->fd, data, len, 0);
	if (n < 0) return zephyr_errno_to_ove(errno);
	if (sent) *sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *buf, size_t len,
		    size_t *received, uint32_t timeout_ms)
{
	if (!sock || !buf) return OVE_ERR_INVALID_PARAM;
	if (!ove_timeout_is_forever(timeout_ms)) {
		struct zsock_timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		zsock_setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO,
				 &tv, sizeof(tv));
	}
	ssize_t n = zsock_recv(sock->fd, buf, len, 0);
	if (n < 0) {
		if (errno == EAGAIN) return OVE_ERR_TIMEOUT;
		return zephyr_errno_to_ove(errno);
	}
	if (n == 0) return OVE_ERR_NET_CLOSED;
	if (received) *received = (size_t)n;
	return OVE_OK;
}

int ove_socket_sendto(ove_socket_t sock, const void *data, size_t len,
		      size_t *sent, const ove_sockaddr_t *dest)
{
	if (!sock || !data || !dest) return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	sockaddr_to_zephyr(dest, &sin);
	ssize_t n = zsock_sendto(sock->fd, data, len, 0,
				 (struct sockaddr *)&sin, sizeof(sin));
	if (n < 0) return zephyr_errno_to_ove(errno);
	if (sent) *sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len,
			size_t *received, ove_sockaddr_t *src,
			uint32_t timeout_ms)
{
	if (!sock || !buf) return OVE_ERR_INVALID_PARAM;
	if (!ove_timeout_is_forever(timeout_ms)) {
		struct zsock_timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		zsock_setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO,
				 &tv, sizeof(tv));
	}
	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);
	ssize_t n = zsock_recvfrom(sock->fd, buf, len, 0,
				   (struct sockaddr *)&sin, &slen);
	if (n < 0) {
		if (errno == EAGAIN) return OVE_ERR_TIMEOUT;
		return zephyr_errno_to_ove(errno);
	}
	if (n == 0) return OVE_ERR_NET_CLOSED;
	if (received) *received = (size_t)n;
	if (src) zephyr_to_sockaddr(&sin, src);
	return OVE_OK;
}

/* ---------- DNS ---------- */

/* DNS with timeout via Zephyr dns_resolve API + semaphore */

static K_SEM_DEFINE(s_dns_sem, 0, 1);
static struct sockaddr_in s_dns_result_addr;
static volatile int       s_dns_done;

static void dns_result_cb(enum dns_resolve_status status,
			  struct dns_addrinfo *info, void *user_data)
{
	(void)user_data;
	if (status == DNS_EAI_INPROGRESS && info) {
		if (info->ai_family == AF_INET) {
			memcpy(&s_dns_result_addr, &info->ai_addr,
			       sizeof(s_dns_result_addr));
			s_dns_done = 1;
		}
	} else if (status == DNS_EAI_ALLDONE) {
		k_sem_give(&s_dns_sem);
	} else {
		s_dns_done = -1;
		k_sem_give(&s_dns_sem);
	}
}

int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr,
		    uint32_t timeout_ms)
{
	if (!hostname || !addr) return OVE_ERR_INVALID_PARAM;
	if (timeout_ms == 0) timeout_ms = 10000;

	s_dns_done = 0;
	k_sem_reset(&s_dns_sem);

	uint16_t dns_id = 0;
	int rc = dns_resolve_name(dns_resolve_get_default(),
				  hostname, DNS_QUERY_TYPE_A,
				  &dns_id, dns_result_cb, NULL,
				  (int32_t)timeout_ms);
	if (rc < 0)
		return OVE_ERR_NET_DNS_FAIL;

	/* Wait for completion with timeout */
	if (k_sem_take(&s_dns_sem, K_MSEC(timeout_ms)) != 0)
		return OVE_ERR_TIMEOUT;

	if (s_dns_done != 1)
		return OVE_ERR_NET_DNS_FAIL;

	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	memcpy(addr->addr, &s_dns_result_addr.sin_addr, 4);
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
