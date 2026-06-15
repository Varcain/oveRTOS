/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM networking stubs.
 *
 * All socket/network functions return OVE_ERR_NOT_SUPPORTED.
 * Prevents link errors when networking modules are enabled.
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef CONFIG_OVE_NET

#include "ove/net.h"
#include <string.h>

int ove_netif_init(ove_netif_t *n, ove_netif_storage_t *s)
{
	(void)n;
	(void)s;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_netif_deinit(ove_netif_t n)
{
	(void)n;
}
int ove_netif_up(ove_netif_t n, const ove_netif_config_t *c)
{
	(void)n;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_netif_down(ove_netif_t n)
{
	(void)n;
}
int ove_netif_get_addr(ove_netif_t n, ove_sockaddr_t *ip, ove_sockaddr_t *mask, ove_sockaddr_t *gw)
{
	(void)n;
	(void)ip;
	(void)mask;
	(void)gw;
	return OVE_ERR_NOT_SUPPORTED;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_netif_create(ove_netif_t *n)
{
	(void)n;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_netif_destroy(ove_netif_t n)
{
	(void)n;
}
#endif

int ove_socket_open(ove_socket_t *s, ove_socket_storage_t *st, ove_af_t af, ove_sock_type_t type)
{
	(void)s;
	(void)st;
	(void)af;
	(void)type;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_socket_close(ove_socket_t s)
{
	(void)s;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_socket_create(ove_socket_t *s, ove_af_t af, ove_sock_type_t type)
{
	(void)s;
	(void)af;
	(void)type;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_socket_destroy(ove_socket_t s)
{
	(void)s;
}
#endif

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint64_t timeout_ns)
{
	(void)sock;
	(void)addr;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	(void)sock;
	(void)addr;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_socket_listen(ove_socket_t sock, int backlog)
{
	(void)sock;
	(void)backlog;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_socket_accept(ove_socket_t sock, ove_socket_t *client, ove_socket_storage_t *client_storage,
		      uint64_t timeout_ns)
{
	(void)sock;
	(void)client;
	(void)client_storage;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_socket_send(ove_socket_t sock, const void *data, size_t len, size_t *sent)
{
	(void)sock;
	(void)data;
	(void)len;
	(void)sent;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received, uint64_t timeout_ns)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)received;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_socket_sendto(ove_socket_t sock, const void *data, size_t len, size_t *sent,
		      const ove_sockaddr_t *dest)
{
	(void)sock;
	(void)data;
	(void)len;
	(void)sent;
	(void)dest;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len, size_t *received,
			ove_sockaddr_t *src, uint64_t timeout_ns)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)received;
	(void)src;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint64_t timeout_ns)
{
	(void)hostname;
	(void)addr;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_NET */
