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
#include "ove_ns_to_ticks.h"

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

#if defined(CONFIG_OVE_LINUX_NET)
#include "lxp/lxp_net.h"
#endif

#include <string.h>

/* Under the FreeRTOS MPU port (the Linux personality) the RTOS-infrastructure tasks
 * created here (the eth RX poller; lwIP's threads via lwip_sys_arch.c) must run
 * PRIVILEGED — they touch the lwIP heap, the ETH DMA descriptors, and MAC registers,
 * and their stacks live in SDRAM where exception (un)stacking needs kernel access.
 * portPRIVILEGE_BIT ORs into the task priority; on the non-MPU port the symbol is
 * undefined -> 0 (a no-op). Mirrors freertos_thread.c / freertos_lnx.c. */
#ifndef portPRIVILEGE_BIT
#define portPRIVILEGE_BIT 0u
#endif

/* ns -> struct timeval with fast path: <4.29 sec inputs use 32-bit
 * divides (single-cycle UDIV on Cortex-M7), >4.29 sec inputs fall
 * back to __aeabi_uldivmod. Inline static so the compiler folds it
 * at each call site. Placed after lwip/sockets.h so `struct timeval`
 * is available. */
static inline void ove_ns_to_timeval(uint64_t ns, struct timeval *tv)
{
	if (ns <= (uint64_t)UINT32_MAX) {
		uint32_t n = (uint32_t)ns;
		tv->tv_sec = (long)(n / 1000000000u);
		tv->tv_usec = (long)((n % 1000000000u) / 1000u);
	} else {
		tv->tv_sec = (long)(ns / 1000000000ULL);
		tv->tv_usec = (long)((ns % 1000000000ULL) / 1000ULL);
	}
}

/* Board provides these — weak defaults for boards without hardware NIC. */
extern err_t ethernetif_init(struct netif *netif) __attribute__((weak));
err_t ethernetif_init(struct netif *netif)
{
	(void)netif;
	return ERR_IF;
}

/* Returns the number of frames delivered to the stack this poll (0 for the no-NIC
 * default). A zero budget asks a native application to drain the driver's usual
 * hardware-sized batch. */
extern int ethernetif_input(struct netif *netif, unsigned budget) __attribute__((weak));
int ethernetif_input(struct netif *netif, unsigned budget)
{
	(void)netif;
	(void)budget;
	return 0;
}

static struct netif s_netif;
static volatile int s_tcpip_ready;

#if defined(CONFIG_OVE_LINUX_NET)
/*
 * Limit bulk traffic to roughly one full-size frame per 8 ms (~1.5 Mbit/s), the
 * throughput class measured with NuttX under the same saturating userspace load.
 * Keep three credits so an idle link can pass a short TCP/ARP burst immediately
 * instead of inserting 8 ms between every frame. One credit is charged per frame
 * and refilled every 8 ms; the 1 ms empty poll keeps sparse-traffic latency low.
 */
#define ETH_RX_TOKEN_BURST 3u
#define ETH_RX_TOKEN_REFILL_MS 8u
#endif

/* Polling task for the Ethernet RX path */
static void eth_rx_task(void *arg)
{
	(void)arg;

#if defined(CONFIG_OVE_LINUX_NET)
	unsigned tokens = ETH_RX_TOKEN_BURST;
	int backlog = 0;
	TickType_t refill_at = xTaskGetTickCount();
	TickType_t refill_ticks = pdMS_TO_TICKS(ETH_RX_TOKEN_REFILL_MS);
	if (refill_ticks == 0)
		refill_ticks = 1;

	for (;;) {
		TickType_t now = xTaskGetTickCount();

		if (tokens == ETH_RX_TOKEN_BURST) {
			/* Discard credit that arrived while the bucket was already full. */
			refill_at = now;
		} else {
			TickType_t elapsed = now - refill_at;
			unsigned added = (unsigned)(elapsed / refill_ticks);
			if (added >= ETH_RX_TOKEN_BURST - tokens) {
				tokens = ETH_RX_TOKEN_BURST;
				refill_at = now;
			} else if (added > 0) {
				tokens += added;
				refill_at += (TickType_t)added * refill_ticks;
			}
		}

		/*
		 * Once a bounded poll consumes every offered credit, assume the DMA
		 * ring is backlogged and accumulate a full burst before polling it
		 * again. This preserves the token rate but coalesces saturated traffic
		 * into one three-frame wakeup per 24 ms instead of one wakeup per 8 ms.
		 * A short poll clears backlog as soon as the ring drains.
		 */
		unsigned threshold = backlog ? ETH_RX_TOKEN_BURST : 1u;
		if (tokens < threshold) {
			TickType_t elapsed = now - refill_at;
			TickType_t wait = (TickType_t)(threshold - tokens) * refill_ticks - elapsed;
			vTaskDelay(wait);
			continue;
		}

		unsigned budget = tokens;
		int n = ethernetif_input(&s_netif, budget);
		if (n > 0) {
			unsigned used = (unsigned)n;
			if (used > budget)
				used = budget;
			tokens -= used;
			backlog = used == budget;

			/* tcpip_input queued these frames before this post. The tcpip
			 * thread therefore makes socket state visible before the equal-
			 * priority personality coordinator retries its parked operation. */
			lxp_sock_kick();
		} else
			backlog = 0;

		if (tokens > 0)
			vTaskDelay(pdMS_TO_TICKS(1u));
	}
#else
	for (;;) {
		(void)ethernetif_input(&s_netif, 0);
		vTaskDelay(pdMS_TO_TICKS(1u));
	}
#endif
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
	case EAGAIN: /* would-block on a non-blocking socket (EWOULDBLOCK == EAGAIN on lwIP) */
		return OVE_ERR_TIMEOUT;
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
	switch (type) {
	case OVE_SOCK_DGRAM:
		return SOCK_DGRAM;
	case OVE_SOCK_RAW:
		return SOCK_RAW;
	default:
		return SOCK_STREAM;
	}
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
	xTaskCreateStatic(eth_rx_task, "eth_rx", 1024, NULL, 4 | portPRIVILEGE_BIT, s_eth_rx_stack,
			  &s_eth_rx_tcb);
#else
	xTaskCreate(eth_rx_task, "eth_rx", 1024, NULL, 4 | portPRIVILEGE_BIT, NULL);
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

/* ove_sockaddr_t.addr[] holds the raw network-order bytes; ip4_addr_t.addr is a
 * network-order u32_t, so a straight 4-byte copy converts either direction. */
static void ove_sa_to_ip4(const ove_sockaddr_t *sa, ip4_addr_t *a)
{
	memcpy(&a->addr, sa->addr, 4);
}

int ove_netif_set_addr(ove_netif_t netif, const ove_sockaddr_t *ip, const ove_sockaddr_t *netmask,
		       const ove_sockaddr_t *gateway)
{
	if (!netif || !netif->lwip_netif)
		return OVE_ERR_INVALID_PARAM;
	struct netif *nif = (struct netif *)netif->lwip_netif;
	ip4_addr_t a;
	if (ip) {
		ove_sa_to_ip4(ip, &a);
		netif_set_ipaddr(nif, &a);
	}
	if (netmask) {
		ove_sa_to_ip4(netmask, &a);
		netif_set_netmask(nif, &a);
	}
	if (gateway) {
		ove_sa_to_ip4(gateway, &a);
		netif_set_gw(nif, &a);
	}
	return OVE_OK;
}

int ove_netif_set_up(ove_netif_t netif, int up)
{
	if (!netif || !netif->lwip_netif)
		return OVE_ERR_INVALID_PARAM;
	struct netif *nif = (struct netif *)netif->lwip_netif;
	if (up)
		netif_set_up(nif);
	else
		netif_set_down(nif);
	return OVE_OK;
}

int ove_netif_get_hwaddr(ove_netif_t netif, uint8_t mac[6])
{
	if (!netif || !netif->lwip_netif)
		return OVE_ERR_INVALID_PARAM;
	struct netif *nif = (struct netif *)netif->lwip_netif;
	memcpy(mac, nif->hwaddr, 6);
	return OVE_OK;
}

int ove_netif_get_flags(ove_netif_t netif, unsigned *flags)
{
	if (!netif || !netif->lwip_netif || !flags)
		return OVE_ERR_INVALID_PARAM;
	struct netif *nif = (struct netif *)netif->lwip_netif;
	unsigned f = 0;
	if (netif_is_up(nif))
		f |= OVE_NETIF_FLAG_UP;
	if (netif_is_link_up(nif))
		f |= OVE_NETIF_FLAG_RUNNING;
	if (nif->flags & NETIF_FLAG_BROADCAST)
		f |= (OVE_NETIF_FLAG_BROADCAST | OVE_NETIF_FLAG_MULTICAST);
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
	struct ove_socket *s = (struct ove_socket *)storage;
	/* proto is the IP protocol (e.g. IPPROTO_ICMP=1 for a raw ping socket); 0 lets
	 * lwIP pick the default for the type. LWIP_RAW must be enabled for SOCK_RAW. */
	int fd = lwip_socket(af_to_lwip(af), type_to_lwip(type), proto);
	if (fd < 0)
		return lwip_errno_to_ove(errno);
	s->fd = fd;
	/* A raw IPv4 ICMP socket (BusyBox ping) carries an app-computed L4 checksum.
	 * This board offloads ICMP checksums to the MAC (CHECKSUM_GEN_ICMP=0), which
	 * only inserts one when the field is zero — see ove_socket_sendto(). */
	s->icmp_raw = (af == OVE_AF_INET && type == OVE_SOCK_RAW && proto == IPPROTO_ICMP);
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

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint64_t timeout_ns)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;

	struct sockaddr_in sin;
	sockaddr_to_lwip(addr, &sin);

	if (ove_timeout_is_forever(timeout_ns)) {
		if (lwip_connect(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
			return lwip_errno_to_ove(errno);
		return OVE_OK;
	}

	/* Bounded connect: go non-blocking, then select() for writability (or
	 * the timeout) and read SO_ERROR for the result. */
	int flags = lwip_fcntl(sock->fd, F_GETFL, 0);
	lwip_fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);

	int result;
	if (lwip_connect(sock->fd, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
		result = OVE_OK; /* completed immediately (e.g. loopback) */
	} else if (errno != EINPROGRESS) {
		result = lwip_errno_to_ove(errno);
	} else {
		fd_set wfds;
		FD_ZERO(&wfds);
		FD_SET(sock->fd, &wfds);
		struct timeval tv;
		ove_ns_to_timeval(timeout_ns, &tv);
		int sr = lwip_select(sock->fd + 1, NULL, &wfds, NULL, &tv);
		if (sr == 0) {
			result = OVE_ERR_TIMEOUT;
		} else if (sr < 0) {
			result = lwip_errno_to_ove(errno);
		} else {
			int soerr = 0;
			socklen_t sl = sizeof(soerr);
			lwip_getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
			result = (soerr == 0) ? OVE_OK : lwip_errno_to_ove(soerr);
		}
	}

	lwip_fcntl(sock->fd, F_SETFL, flags); /* restore blocking mode */
	return result;
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
		      uint64_t timeout_ns)
{
	if (!sock || !client || !client_storage)
		return OVE_ERR_INVALID_PARAM;

	/* Bound the accept wait via SO_RCVTIMEO (lwip_accept honours it). */
	if (!ove_timeout_is_forever(timeout_ns)) {
		struct timeval tv;
		ove_ns_to_timeval(timeout_ns, &tv);
		lwip_setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	int fd = lwip_accept(sock->fd, NULL, NULL);
	if (fd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return OVE_ERR_TIMEOUT;
		return lwip_errno_to_ove(errno);
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
	ssize_t n = lwip_send(sock->fd, data, len, 0);
	if (n < 0)
		return lwip_errno_to_ove(errno);
	if (sent)
		*sent = (size_t)n;
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received, uint64_t timeout_ns)
{
	if (!sock || !buf)
		return OVE_ERR_INVALID_PARAM;

	/* Set receive timeout via SO_RCVTIMEO */
	if (!ove_timeout_is_forever(timeout_ns)) {
		struct timeval tv;
		ove_ns_to_timeval(timeout_ns, &tv);
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
	const void *sbuf = data;
	/* HW ICMP-checksum offload (CHECKSUM_GEN_ICMP=0) only inserts a checksum when
	 * the field is zero. BusyBox ping fills it in software, so the MAC leaves a
	 * bad (zero-on-wire) csum and the peer drops the echo. Send a copy with the
	 * ICMP checksum (offset 2) cleared so the MAC computes it from scratch. */
	static uint8_t icmp_tx[1518];
	if (sock->icmp_raw && len >= 4 && len <= sizeof(icmp_tx)) {
		memcpy(icmp_tx, data, len);
		icmp_tx[2] = 0;
		icmp_tx[3] = 0;
		sbuf = icmp_tx;
	}
	ssize_t n = lwip_sendto(sock->fd, sbuf, len, 0, (struct sockaddr *)&sin, sizeof(sin));
	if (n < 0)
		return lwip_errno_to_ove(errno);
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
		struct timeval tv;
		ove_ns_to_timeval(timeout_ns, &tv);
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

/* ---------- Non-blocking readiness (drives the Linux-personality park/retry) ---------- */

int ove_socket_set_nonblock(ove_socket_t sock, int nonblock)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int flags = lwip_fcntl(sock->fd, F_GETFL, 0);
	if (flags < 0)
		return lwip_errno_to_ove(errno);
	flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	if (lwip_fcntl(sock->fd, F_SETFL, flags) < 0)
		return lwip_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_poll(ove_socket_t sock, unsigned events, unsigned *revents, uint64_t timeout_ns)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	fd_set rfds, wfds, efds;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	if (events & OVE_SOCK_POLLIN)
		FD_SET(sock->fd, &rfds);
	if (events & OVE_SOCK_POLLOUT)
		FD_SET(sock->fd, &wfds);
	FD_SET(sock->fd, &efds);
	struct timeval tv, *ptv = NULL;
	if (!ove_timeout_is_forever(timeout_ns)) {
		ove_ns_to_timeval(timeout_ns, &tv);
		ptv = &tv;
	}
	int sr = lwip_select(sock->fd + 1, &rfds, &wfds, &efds, ptv);
	if (sr < 0)
		return lwip_errno_to_ove(errno);
	unsigned re = 0;
	if (FD_ISSET(sock->fd, &rfds))
		re |= OVE_SOCK_POLLIN;
	if (FD_ISSET(sock->fd, &wfds))
		re |= OVE_SOCK_POLLOUT;
	if (FD_ISSET(sock->fd, &efds))
		re |= OVE_SOCK_POLLERR;
	if (revents)
		*revents = re;
	return OVE_OK;
}

int ove_socket_shutdown(ove_socket_t sock, int how)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int lh = (how == OVE_SHUT_RD) ? SHUT_RD : (how == OVE_SHUT_WR) ? SHUT_WR : SHUT_RDWR;
	if (lwip_shutdown(sock->fd, lh) < 0)
		return lwip_errno_to_ove(errno);
	return OVE_OK;
}

int ove_socket_getsockname(ove_socket_t sock, ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	socklen_t sl = sizeof(sin);
	if (lwip_getsockname(sock->fd, (struct sockaddr *)&sin, &sl) < 0)
		return lwip_errno_to_ove(errno);
	lwip_to_sockaddr(&sin, addr);
	return OVE_OK;
}

int ove_socket_getpeername(ove_socket_t sock, ove_sockaddr_t *addr)
{
	if (!sock || !addr)
		return OVE_ERR_INVALID_PARAM;
	struct sockaddr_in sin;
	socklen_t sl = sizeof(sin);
	if (lwip_getpeername(sock->fd, (struct sockaddr *)&sin, &sl) < 0)
		return lwip_errno_to_ove(errno);
	lwip_to_sockaddr(&sin, addr);
	return OVE_OK;
}

int ove_socket_get_error(ove_socket_t sock)
{
	if (!sock)
		return OVE_ERR_INVALID_PARAM;
	int soerr = 0;
	socklen_t sl = sizeof(soerr);
	if (lwip_getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0)
		return lwip_errno_to_ove(errno);
	return soerr ? lwip_errno_to_ove(soerr) : OVE_OK;
}

/* ---------- DNS ---------- */

/* DNS with timeout via lwIP raw API + semaphore */

/* DNS shared state is guarded by s_dns_mutex (one resolve in flight at a
 * time) plus a per-call generation token passed through lwIP's callback_arg.
 * lwIP cannot cancel a pending query, so a request that we timed out on may
 * still complete later; the callback checks its token against s_dns_gen and
 * drops the result if a newer call has superseded it — preventing a stale
 * completion from corrupting the next resolve or leaving a phantom sem token. */
static SemaphoreHandle_t s_dns_mutex;
static StaticSemaphore_t s_dns_mutex_buf;
static SemaphoreHandle_t s_dns_sem;
static StaticSemaphore_t s_dns_sem_buf;
static ip_addr_t s_dns_result;
static volatile int s_dns_done;
static volatile uintptr_t s_dns_gen;
static int s_dns_inited;

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
	(void)name;
	/* Ignore a late completion whose call already timed out / was
	 * superseded — its generation no longer matches the active one. */
	if ((uintptr_t)callback_arg != s_dns_gen)
		return;
	if (ipaddr) {
		s_dns_result = *ipaddr;
		s_dns_done = 1;
	} else {
		s_dns_done = -1;
	}
	xSemaphoreGive(s_dns_sem);
}

int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint64_t timeout_ns)
{
	if (!hostname || !addr)
		return OVE_ERR_INVALID_PARAM;
	if (timeout_ns == 0)
		timeout_ns = OVE_SEC(10);

	if (!s_dns_inited) {
		s_dns_mutex = xSemaphoreCreateMutexStatic(&s_dns_mutex_buf);
		s_dns_sem = xSemaphoreCreateBinaryStatic(&s_dns_sem_buf);
		s_dns_inited = 1;
	}

	/* Serialize: the result/done/sem globals have a single owner per call. */
	xSemaphoreTake(s_dns_mutex, portMAX_DELAY);

	/* Unique non-zero generation for this call; the callback echoes it back. */
	uintptr_t gen = ++s_dns_gen;
	if (gen == 0)
		gen = ++s_dns_gen;
	s_dns_done = 0;
	/* Drop any stale token a prior late completion may have left. */
	xSemaphoreTake(s_dns_sem, 0);

	ip_addr_t resolved;
	int rc;
	err_t err = dns_gethostbyname(hostname, &resolved, dns_found_cb, (void *)gen);
	if (err == ERR_OK) {
		/* Already cached — result is in 'resolved' */
		memset(addr, 0, sizeof(*addr));
		addr->family = OVE_AF_INET;
		memcpy(addr->addr, &resolved.addr, 4);
		rc = OVE_OK;
	} else if (err != ERR_INPROGRESS) {
		rc = OVE_ERR_NET_DNS_FAIL;
	} else if (xSemaphoreTake(s_dns_sem, ove_ns_to_ticks(timeout_ns)) != pdTRUE) {
		/* Timed out — invalidate this call so the eventual late callback
		 * is recognized as stale and dropped. */
		s_dns_gen++;
		rc = OVE_ERR_TIMEOUT;
	} else if (s_dns_done != 1) {
		rc = OVE_ERR_NET_DNS_FAIL;
	} else {
		memset(addr, 0, sizeof(*addr));
		addr->family = OVE_AF_INET;
		memcpy(addr->addr, &s_dns_result.addr, 4);
		rc = OVE_OK;
	}

	xSemaphoreGive(s_dns_mutex);
	return rc;
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
