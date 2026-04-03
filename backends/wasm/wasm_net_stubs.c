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
{ (void)n; (void)s; return OVE_ERR_NOT_SUPPORTED; }
void ove_netif_deinit(ove_netif_t n) { (void)n; }
int ove_netif_up(ove_netif_t n, const ove_netif_config_t *c)
{ (void)n; (void)c; return OVE_ERR_NOT_SUPPORTED; }
void ove_netif_down(ove_netif_t n) { (void)n; }
int ove_netif_get_addr(ove_netif_t n, ove_sockaddr_t *ip,
		       ove_sockaddr_t *mask, ove_sockaddr_t *gw)
{ (void)n; (void)ip; (void)mask; (void)gw; return OVE_ERR_NOT_SUPPORTED; }

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_netif_create(ove_netif_t *n)
{ (void)n; return OVE_ERR_NOT_SUPPORTED; }
void ove_netif_destroy(ove_netif_t n) { (void)n; }
#endif

int ove_socket_open(ove_socket_t *s, ove_socket_storage_t *st,
		    ove_af_t af, ove_sock_type_t type)
{ (void)s; (void)st; (void)af; (void)type; return OVE_ERR_NOT_SUPPORTED; }
void ove_socket_close(ove_socket_t s) { (void)s; }

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_socket_create(ove_socket_t *s, ove_af_t af, ove_sock_type_t type)
{ (void)s; (void)af; (void)type; return OVE_ERR_NOT_SUPPORTED; }
void ove_socket_destroy(ove_socket_t s) { (void)s; }
#endif

int ove_socket_connect(ove_socket_t s, const ove_sockaddr_t *a,
		       uint32_t t)
{ (void)s; (void)a; (void)t; return OVE_ERR_NOT_SUPPORTED; }
int ove_socket_bind(ove_socket_t s, const ove_sockaddr_t *a)
{ (void)s; (void)a; return OVE_ERR_NOT_SUPPORTED; }
int ove_socket_listen(ove_socket_t s, int b)
{ (void)s; (void)b; return OVE_ERR_NOT_SUPPORTED; }
int ove_socket_accept(ove_socket_t s, ove_socket_t *c,
		      ove_socket_storage_t *st, ove_sockaddr_t *a,
		      uint32_t t)
{ (void)s; (void)c; (void)st; (void)a; (void)t; return OVE_ERR_NOT_SUPPORTED; }
int ove_socket_send(ove_socket_t s, const void *d, size_t l,
		    size_t *sent, uint32_t t)
{ (void)s; (void)d; (void)l; (void)sent; (void)t; return OVE_ERR_NOT_SUPPORTED; }
int ove_socket_recv(ove_socket_t s, void *b, size_t l,
		    size_t *recv_len, uint32_t t)
{ (void)s; (void)b; (void)l; (void)recv_len; (void)t; return OVE_ERR_NOT_SUPPORTED; }
int ove_socket_sendto(ove_socket_t s, const void *d, size_t l,
		      const ove_sockaddr_t *a, size_t *sent, uint32_t t)
{ (void)s; (void)d; (void)l; (void)a; (void)sent; (void)t; return OVE_ERR_NOT_SUPPORTED; }
int ove_socket_recvfrom(ove_socket_t s, void *b, size_t l,
			ove_sockaddr_t *a, size_t *recv_len, uint32_t t)
{ (void)s; (void)b; (void)l; (void)a; (void)recv_len; (void)t; return OVE_ERR_NOT_SUPPORTED; }
int ove_dns_resolve(const char *h, ove_sockaddr_t *a,
		    ove_af_t af, uint32_t t)
{ (void)h; (void)a; (void)af; (void)t; return OVE_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_OVE_NET */
