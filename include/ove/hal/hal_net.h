/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_NET_H
#define OVE_HAL_NET_H

/**
 * @defgroup ove_hal_net Networking HAL
 * @brief Backend-implemented networking primitives.
 *
 * Each RTOS backend provides these functions to bridge the portable
 * oveRTOS socket API to the platform's native TCP/IP stack.
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_NET

/** @brief Backend: open a socket (called by ove_socket_open). */
int  ove_hal_socket_open(ove_socket_t sock, int af, int type);

/** @brief Backend: close a socket. */
void ove_hal_socket_close(ove_socket_t sock);

/** @brief Backend: connect to a remote address. */
int  ove_hal_socket_connect(ove_socket_t sock, const void *addr,
				uint32_t timeout_ms);

/** @brief Backend: bind to a local address. */
int  ove_hal_socket_bind(ove_socket_t sock, const void *addr);

/** @brief Backend: listen for incoming connections. */
int  ove_hal_socket_listen(ove_socket_t sock, int backlog);

/** @brief Backend: accept an incoming connection. */
int  ove_hal_socket_accept(ove_socket_t sock, ove_socket_t client,
			       uint32_t timeout_ms);

/** @brief Backend: send data on a connected socket. */
int  ove_hal_socket_send(ove_socket_t sock, const void *data,
			     size_t len, size_t *sent);

/** @brief Backend: receive data from a connected socket. */
int  ove_hal_socket_recv(ove_socket_t sock, void *buf, size_t len,
			     size_t *received, uint32_t timeout_ms);

/** @brief Backend: send a datagram to a destination. */
int  ove_hal_socket_sendto(ove_socket_t sock, const void *data,
			       size_t len, size_t *sent, const void *dest);

/** @brief Backend: receive a datagram and sender address. */
int  ove_hal_socket_recvfrom(ove_socket_t sock, void *buf, size_t len,
				 size_t *received, void *src,
				 uint32_t timeout_ms);

/** @brief Backend: resolve a hostname to an address. */
int  ove_hal_dns_resolve(const char *hostname, void *addr,
			     uint32_t timeout_ms);

/** @brief Backend: initialise the network interface. */
int  ove_hal_netif_init(ove_netif_t netif);

/** @brief Backend: de-initialise the network interface. */
void ove_hal_netif_deinit(ove_netif_t netif);

/** @brief Backend: bring the network interface up. */
int  ove_hal_netif_up(ove_netif_t netif, const void *cfg);

/** @brief Backend: tear down the network interface. */
void ove_hal_netif_down(ove_netif_t netif);

#endif /* CONFIG_OVE_NET */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_NET_H */
