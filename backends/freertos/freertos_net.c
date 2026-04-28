/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * FreeRTOS/lwIP networking backend.
 *
 * Wraps the lwIP socket API (lwip_socket, lwip_connect, etc.) to
 * implement the oveRTOS socket interface on FreeRTOS targets.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

#include <string.h>

/* Board provides these — weak defaults for boards without hardware NIC. */
extern err_t ethernetif_init(struct netif *netif) __attribute__((weak));
err_t ethernetif_init(struct netif *netif)
{
	(void)netif;
	return ERR_IF;
}

extern void ethernetif_input(struct netif *netif) __attribute__((weak));
void ethernetif_input(struct netif *netif)
{
	(void)netif;
}

static struct netif s_netif;
static volatile int s_tcpip_ready;

/* Polling task for the Ethernet RX path */
static void eth_rx_task(void *arg)
{
	(void)arg;
	for (;;) {
		ethernetif_input(&s_netif);
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

/* ---------- helpers ---------- */

static int lwip_errno_to_ove(int err)
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
	default:
		return OVE_ERR_NOT_SUPPORTED;
	}
}

static void sockaddr_to_lwip(const ove_sockaddr_t *ove, struct sockaddr_in *sin)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = lwip_htons(ove->port);
	memcpy(&sin->sin_addr.s_addr, ove->addr, 4);
	sin->sin_len = sizeof(*sin);
}

static void lwip_to_sockaddr(const struct sockaddr_in *sin, ove_sockaddr_t *ove)
{
	memset(ove, 0, sizeof(*ove));
	ove->family = OVE_AF_INET;
	ove->port = lwip_ntohs(sin->sin_port);
	memcpy(ove->addr, &sin->sin_addr.s_addr, 4);
}

static int af_to_lwip(ove_af_t af)
{
	(void)af;
	return AF_INET; /* lwIP: IPv4 only for now */
}

static int type_to_lwip(ove_sock_type_t type)
{
	return (type == OVE_SOCK_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
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
	if (netif) {
		netif->initialized = 0;
	}
}

static void tcpip_init_done(void *arg)
{
	(void)arg;
	s_tcpip_ready = 1;
}

int ove_netif_up(ove_netif_t netif, const ove_netif_config_t *cfg)
{
	if (!netif)
		return OVE_ERR_INVALID_PARAM;

	/* Initialise the lwIP TCP/IP thread (once) */
	if (!s_tcpip_ready) {
		tcpip_init(tcpip_init_done, NULL);
		while (!s_tcpip_ready) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	ip4_addr_t ipaddr, netmask, gw;

	if (cfg && !cfg->use_dhcp) {
		/* Static IP from oveRTOS sockaddr */
		IP4_ADDR(&ipaddr, cfg->static_ip.addr[0], cfg->static_ip.addr[1],
			 cfg->static_ip.addr[2], cfg->static_ip.addr[3]);
		IP4_ADDR(&netmask, cfg->netmask.addr[0], cfg->netmask.addr[1], cfg->netmask.addr[2],
			 cfg->netmask.addr[3]);
		IP4_ADDR(&gw, cfg->gateway.addr[0], cfg->gateway.addr[1], cfg->gateway.addr[2],
			 cfg->gateway.addr[3]);
	} else {
		ip4_addr_set_zero(&ipaddr);
		ip4_addr_set_zero(&netmask);
		ip4_addr_set_zero(&gw);
	}

	/* All netif operations must be done under the tcpip core lock */
	LOCK_TCPIP_CORE();

	struct netif *nif =
		netif_add(&s_netif, &ipaddr, &netmask, &gw, NULL, ethernetif_init, tcpip_input);
	if (!nif) {
		UNLOCK_TCPIP_CORE();
		return OVE_ERR_NOT_SUPPORTED;
	}

	netif_set_default(&s_netif);
	netif_set_up(&s_netif);

	if (cfg && cfg->use_dhcp) {
		dhcp_start(&s_netif);
	}

	/* Configure DNS server */
	if (cfg && (cfg->dns.addr[0] | cfg->dns.addr[1] | cfg->dns.addr[2] | cfg->dns.addr[3])) {
		ip_addr_t dns_addr;
		IP4_ADDR(ip_2_ip4(&dns_addr), cfg->dns.addr[0], cfg->dns.addr[1], cfg->dns.addr[2],
			 cfg->dns.addr[3]);
		dns_setserver(0, &dns_addr);
	}

	UNLOCK_TCPIP_CORE();

	netif->lwip_netif = &s_netif;

	/* Start the Ethernet RX polling task */
#ifdef CONFIG_OVE_ZERO_HEAP
	static StaticTask_t s_eth_rx_tcb;
	static StackType_t s_eth_rx_stack[1024];
	xTaskCreateStatic(eth_rx_task, "eth_rx", 1024, NULL, 4, s_eth_rx_stack, &s_eth_rx_tcb);
#else
	xTaskCreate(eth_rx_task, "eth_rx", 1024, NULL, 4, NULL);
#endif

	return OVE_OK;
}

void ove_netif_down(ove_netif_t netif)
{
	if (!netif)
		return;
	if (netif->lwip_netif) {
		netif_set_down(netif->lwip_netif);
		netif->lwip_netif = NULL;
	}
}

int ove_netif_get_addr(ove_netif_t netif, ove_sockaddr_t *ip, ove_sockaddr_t *gateway,
		       ove_sockaddr_t *netmask)
{
	if (!netif || !netif->lwip_netif)
		return OVE_ERR_INVALID_PARAM;

	struct netif *nif = (struct netif *)netif->lwip_netif;

	if (ip) {
		memset(ip, 0, sizeof(*ip));
		ip->family = OVE_AF_INET;
		const ip4_addr_t *a = netif_ip4_addr(nif);
		memcpy(ip->addr, &a->addr, 4);
	}
	if (gateway) {
		memset(gateway, 0, sizeof(*gateway));
		gateway->family = OVE_AF_INET;
		const ip4_addr_t *g = netif_ip4_gw(nif);
		memcpy(gateway->addr, &g->addr, 4);
	}
	if (netmask) {
		memset(netmask, 0, sizeof(*netmask));
		netmask->family = OVE_AF_INET;
		const ip4_addr_t *m = netif_ip4_netmask(nif);
		memcpy(netmask->addr, &m->addr, 4);
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
	struct ove_socket *s = (struct ove_socket *)storage;
	int fd = lwip_socket(af_to_lwip(af), type_to_lwip(type), 0);
	if (fd < 0)
		return lwip_errno_to_ove(errno);
	s->fd = fd;
	*sock = s;
	return OVE_OK;
}

void ove_socket_close(ove_socket_t sock)
{
	if (sock && sock->fd >= 0) {
		lwip_close(sock->fd);
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
	int fd = lwip_socket(af_to_lwip(af), type_to_lwip(type), 0);
	if (fd < 0) {
		OVE_BACKEND_FREE(s);
		return lwip_errno_to_ove(errno);
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
			lwip_close(sock->fd);
		OVE_BACKEND_FREE(sock);
	}
}
#endif

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint32_t timeout_ms)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	(void)timeout_ms; /* lwIP connect is blocking */

	struct sockaddr_in sin;
	sockaddr_to_lwip(addr, &sin);

	if (lwip_connect(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		return lwip_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	sockaddr_to_lwip(addr, &sin);
	if (lwip_bind(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		return lwip_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_listen(ove_socket_t sock, int backlog)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	if (lwip_listen(sock->fd, backlog) < 0)
		return lwip_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_accept(ove_socket_t sock, ove_socket_t *client, ove_socket_storage_t *client_storage,
		      uint32_t timeout_ms)
{
	if (!sock || !client || !client_storage)
		return OVE_ERR_INVALID_PARAM;
	(void)timeout_ms;

	int fd = lwip_accept(sock->fd, NULL, NULL);
	if (fd < 0)
		return lwip_errno_to_ove(errno);

	struct ove_socket *cs = (struct ove_socket *)client_storage;
	cs->fd = fd;
	*client = cs;
	return OVE_OK;
}

int ove_socket_send(ove_socket_t sock, const void *data, size_t len, size_t *sent)
{
	if (!sock || !data)
		return OVE_ERR_INVALID_PARAM;
	ssize_t n = lwip_send(sock->fd, data, len, 0);
	if (n < 0)
		return lwip_errno_to_ove(errno);
	if (sent)
		*sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received, uint32_t timeout_ms)
{
	if (!sock || !buf)
		return OVE_ERR_INVALID_PARAM;

	/* Set receive timeout via SO_RCVTIMEO */
	if (!ove_timeout_is_forever(timeout_ms)) {
		struct timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		lwip_setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	ssize_t n = lwip_recv(sock->fd, buf, len, 0);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return OVE_ERR_TIMEOUT;
		return lwip_errno_to_ove(errno);
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
	sockaddr_to_lwip(dest, &sin);
	ssize_t n = lwip_sendto(sock->fd, data, len, 0, (struct sockaddr *)&sin, sizeof(sin));
	if (n < 0)
		return lwip_errno_to_ove(errno);
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
		struct timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		lwip_setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);
	ssize_t n = lwip_recvfrom(sock->fd, buf, len, 0, (struct sockaddr *)&sin, &slen);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return OVE_ERR_TIMEOUT;
		return lwip_errno_to_ove(errno);
	}
	if (n == 0)
		return OVE_ERR_NET_CLOSED;
	if (received)
		*received = (size_t)n;
	if (src)
		lwip_to_sockaddr(&sin, src);
	return OVE_OK;
}

/* ---------- DNS ---------- */

/* DNS with timeout via lwIP raw API + semaphore */

static SemaphoreHandle_t s_dns_sem;
static StaticSemaphore_t s_dns_sem_buf;
static ip_addr_t s_dns_result;
static volatile int s_dns_done;
static int s_dns_inited;

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
	(void)name;
	(void)callback_arg;
	if (ipaddr) {
		s_dns_result = *ipaddr;
		s_dns_done = 1;
	} else {
		s_dns_done = -1;
	}
	xSemaphoreGive(s_dns_sem);
}

int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint32_t timeout_ms)
{
	if (!hostname || !addr)
		return OVE_ERR_INVALID_PARAM;
	if (timeout_ms == 0)
		timeout_ms = 10000;

	if (!s_dns_inited) {
		s_dns_sem = xSemaphoreCreateBinaryStatic(&s_dns_sem_buf);
		s_dns_inited = 1;
	}

	s_dns_done = 0;
	ip_addr_t resolved;

	err_t err = dns_gethostbyname(hostname, &resolved, dns_found_cb, NULL);
	if (err == ERR_OK) {
		/* Already cached — result is in 'resolved' */
		memset(addr, 0, sizeof(*addr));
		addr->family = OVE_AF_INET;
		memcpy(addr->addr, &resolved.addr, 4);
		return OVE_OK;
	}

	if (err != ERR_INPROGRESS)
		return OVE_ERR_NET_DNS_FAIL;

	/* Wait for callback with timeout */
	if (xSemaphoreTake(s_dns_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
		return OVE_ERR_TIMEOUT;

	if (s_dns_done != 1)
		return OVE_ERR_NET_DNS_FAIL;

	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	memcpy(addr->addr, &s_dns_result.addr, 4);
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
