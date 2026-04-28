/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NET_H
#define OVE_NET_H

/**
 * @defgroup ove_net Networking
 * @brief BSD-like socket API, DNS resolution, and network interface control.
 *
 * Provides TCP/UDP sockets, DNS name resolution, and network interface
 * management.  Each RTOS backend implements the socket layer using its
 * native TCP/IP stack (POSIX sockets, lwIP, Zephyr net, NuttX sockets).
 *
 * @note Requires @c CONFIG_OVE_NET.  When the option is disabled every
 *       function is replaced by a no-op stub that returns
 *       @c OVE_ERR_NOT_SUPPORTED.
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Types (always visible) ──────────────────────────────────────── */

/** @brief Socket type. */
#ifdef __ZIG_CIMPORT__ /* @cond ZIG_ABI */
/* Zig @cImport uses clang which does not default to -fshort-enums on ARM.
 * Use fixed-width types so the struct layout matches the ARM EABI ABI. */
typedef uint8_t ove_sock_type_t;
#define OVE_SOCK_STREAM ((ove_sock_type_t)1)
#define OVE_SOCK_DGRAM ((ove_sock_type_t)2)
typedef uint8_t ove_af_t;
#define OVE_AF_INET ((ove_af_t)2)
#define OVE_AF_INET6 ((ove_af_t)10)
#else /* @endcond */
typedef enum {
	OVE_SOCK_STREAM = 1, /**< Reliable byte-stream (TCP). */
	OVE_SOCK_DGRAM = 2,  /**< Connectionless datagrams (UDP). */
} ove_sock_type_t;

/** @brief Address family. */
typedef enum {
	OVE_AF_INET = 2,   /**< IPv4. */
	OVE_AF_INET6 = 10, /**< IPv6. */
} ove_af_t;
#endif

/**
 * @brief Generic socket address (large enough for IPv4 or IPv6).
 */
typedef struct {
	ove_af_t family;  /**< Address family (OVE_AF_INET or OVE_AF_INET6). */
	uint16_t port;	  /**< Port number in host byte order. */
	uint8_t addr[16]; /**< Address bytes (4 for IPv4, 16 for IPv6). */
} ove_sockaddr_t;

/**
 * @brief Network interface configuration.
 */
typedef struct {
	int use_dhcp;		  /**< Non-zero to use DHCP; zero for static. */
	ove_sockaddr_t static_ip; /**< Static IP (ignored if use_dhcp). */
	ove_sockaddr_t gateway;	  /**< Default gateway (ignored if use_dhcp). */
	ove_sockaddr_t netmask;	  /**< Subnet mask (ignored if use_dhcp). */
	ove_sockaddr_t dns;	  /**< DNS server (0.0.0.0 to skip). */
} ove_netif_config_t;

#include "ove/storage.h"

#ifdef CONFIG_OVE_NET

/* ── Network interface ───────────────────────────────────────────── */

/**
 * @brief Initialise a network interface from caller-supplied storage.
 *
 * @param[out] netif   Handle written on success.
 * @param[in]  storage Caller-allocated storage.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_netif_init(ove_netif_t *netif, ove_netif_storage_t *storage);

/**
 * @brief De-initialise a network interface.
 *
 * @param[in] netif Handle returned by ove_netif_init().
 */
void ove_netif_deinit(ove_netif_t netif);

/**
 * @brief Bring the network interface up.
 *
 * @param[in] netif Handle returned by ove_netif_init().
 * @param[in] cfg   Interface configuration (DHCP or static).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_netif_up(ove_netif_t netif, const ove_netif_config_t *cfg);

/**
 * @brief Tear down the network interface.
 *
 * @param[in] netif Handle returned by ove_netif_init().
 */
void ove_netif_down(ove_netif_t netif);

/**
 * @brief Query the current addresses of a network interface.
 *
 * @param[in]  netif   Handle returned by ove_netif_init().
 * @param[out] ip      Current IPv4 address (may be NULL).
 * @param[out] gateway Current gateway address (may be NULL).
 * @param[out] netmask Current subnet mask (may be NULL).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_netif_get_addr(ove_netif_t netif, ove_sockaddr_t *ip, ove_sockaddr_t *gateway,
		       ove_sockaddr_t *netmask);

#ifdef OVE_HEAP_NET
/**
 * @brief Heap-allocate and initialise a network interface.
 *
 * @param[out] netif Handle written on success.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_netif_create(ove_netif_t *netif);

/**
 * @brief Destroy a heap-allocated network interface.
 *
 * @param[in] netif Handle returned by ove_netif_create().
 */
void ove_netif_destroy(ove_netif_t netif);
#endif /* OVE_HEAP_NET */

/* ── Sockets ─────────────────────────────────────────────────────── */

/**
 * @brief Open a socket from caller-supplied storage.
 *
 * @param[out] sock    Handle written on success.
 * @param[in]  storage Caller-allocated storage.
 * @param[in]  af      Address family.
 * @param[in]  type    Socket type (TCP or UDP).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_open(ove_socket_t *sock, ove_socket_storage_t *storage, ove_af_t af,
		    ove_sock_type_t type);

/**
 * @brief Close a socket.
 *
 * @param[in] sock Handle returned by ove_socket_open().
 */
void ove_socket_close(ove_socket_t sock);

/**
 * @brief Connect a socket to a remote address.
 *
 * @param[in] sock       Socket handle.
 * @param[in] addr       Remote address.
 * @param[in] timeout_ms Timeout in milliseconds (OVE_WAIT_FOREVER to block).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint32_t timeout_ms);

/**
 * @brief Bind a socket to a local address.
 *
 * @param[in] sock Socket handle.
 * @param[in] addr Local address to bind.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr);

/**
 * @brief Mark a bound socket as listening for incoming connections.
 *
 * @param[in] sock    Socket handle.
 * @param[in] backlog Maximum pending connection queue length.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_listen(ove_socket_t sock, int backlog);

/**
 * @brief Accept an incoming connection on a listening socket.
 *
 * @param[in]  sock           Listening socket handle.
 * @param[out] client         Handle for the accepted connection.
 * @param[in]  client_storage Caller-allocated storage for the new socket.
 * @param[in]  timeout_ms     Timeout in milliseconds.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_accept(ove_socket_t sock, ove_socket_t *client, ove_socket_storage_t *client_storage,
		      uint32_t timeout_ms);

/**
 * @brief Send data on a connected socket.
 *
 * @param[in]  sock Socket handle.
 * @param[in]  data Pointer to data to send.
 * @param[in]  len  Number of bytes to send.
 * @param[out] sent Number of bytes actually sent (may be NULL).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_send(ove_socket_t sock, const void *data, size_t len, size_t *sent);

/**
 * @brief Receive data from a connected socket.
 *
 * @param[in]  sock       Socket handle.
 * @param[out] buf        Buffer to receive into.
 * @param[in]  len        Buffer size in bytes.
 * @param[out] received   Number of bytes received (may be NULL).
 * @param[in]  timeout_ms Timeout in milliseconds.
 * @return OVE_OK on success, OVE_ERR_NET_CLOSED if peer closed.
 */
int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received,
		    uint32_t timeout_ms);

/**
 * @brief Send a datagram to a specific destination.
 *
 * @param[in]  sock Socket handle (UDP).
 * @param[in]  data Pointer to data to send.
 * @param[in]  len  Number of bytes to send.
 * @param[out] sent Number of bytes actually sent (may be NULL).
 * @param[in]  dest Destination address.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_sendto(ove_socket_t sock, const void *data, size_t len, size_t *sent,
		      const ove_sockaddr_t *dest);

/**
 * @brief Receive a datagram and the sender's address.
 *
 * @param[in]  sock       Socket handle (UDP).
 * @param[out] buf        Buffer to receive into.
 * @param[in]  len        Buffer size in bytes.
 * @param[out] received   Number of bytes received (may be NULL).
 * @param[out] src        Filled with sender's address (may be NULL).
 * @param[in]  timeout_ms Timeout in milliseconds.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len, size_t *received,
			ove_sockaddr_t *src, uint32_t timeout_ms);

#ifdef OVE_HEAP_NET
/**
 * @brief Heap-allocate and open a socket.
 *
 * @param[out] sock Handle written on success.
 * @param[in]  af   Address family.
 * @param[in]  type Socket type.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_create(ove_socket_t *sock, ove_af_t af, ove_sock_type_t type);

/**
 * @brief Destroy a heap-allocated socket.
 *
 * @param[in] sock Handle returned by ove_socket_create().
 */
void ove_socket_destroy(ove_socket_t sock);
#endif /* OVE_HEAP_NET */

/* ── DNS ─────────────────────────────────────────────────────────── */

/**
 * @brief Resolve a hostname to an address.
 *
 * @param[in]  hostname   Null-terminated hostname string.
 * @param[out] addr       Resolved address written here.
 * @param[in]  timeout_ms Timeout in milliseconds.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint32_t timeout_ms);

/* ── Helpers ─────────────────────────────────────────────────────── */

/**
 * @brief Fill a sockaddr from IPv4 address components.
 *
 * @param[out] addr Destination sockaddr.
 * @param[in]  a    First octet.
 * @param[in]  b    Second octet.
 * @param[in]  c    Third octet.
 * @param[in]  d    Fourth octet.
 * @param[in]  port Port number in host byte order.
 */
void ove_sockaddr_ipv4(ove_sockaddr_t *addr, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
		       uint16_t port);

#else /* !CONFIG_OVE_NET */

/** @cond INTERNAL */
/* Provide dummy storage types so the disabled inline stubs compile. */
#ifndef CONFIG_OVE_NET
typedef struct {
	uint8_t _unused;
} ove_socket_storage_t;
typedef struct {
	uint8_t _unused;
} ove_netif_storage_t;
#endif

static inline int ove_netif_init(ove_netif_t *netif, ove_netif_storage_t *storage)
{
	(void)netif;
	(void)storage;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_netif_deinit(ove_netif_t netif)
{
	(void)netif;
}
static inline int ove_netif_up(ove_netif_t netif, const ove_netif_config_t *cfg)
{
	(void)netif;
	(void)cfg;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_netif_down(ove_netif_t netif)
{
	(void)netif;
}
static inline int ove_netif_get_addr(ove_netif_t netif, ove_sockaddr_t *ip, ove_sockaddr_t *gw,
				     ove_sockaddr_t *nm)
{
	(void)netif;
	(void)ip;
	(void)gw;
	(void)nm;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_open(ove_socket_t *sock, ove_socket_storage_t *storage, ove_af_t af,
				  ove_sock_type_t type)
{
	(void)sock;
	(void)storage;
	(void)af;
	(void)type;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_socket_close(ove_socket_t sock)
{
	(void)sock;
}
static inline int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr,
				     uint32_t timeout_ms)
{
	(void)sock;
	(void)addr;
	(void)timeout_ms;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	(void)sock;
	(void)addr;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_listen(ove_socket_t sock, int backlog)
{
	(void)sock;
	(void)backlog;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_accept(ove_socket_t sock, ove_socket_t *client,
				    ove_socket_storage_t *client_storage, uint32_t timeout_ms)
{
	(void)sock;
	(void)client;
	(void)client_storage;
	(void)timeout_ms;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_send(ove_socket_t sock, const void *data, size_t len, size_t *sent)
{
	(void)sock;
	(void)data;
	(void)len;
	(void)sent;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received,
				  uint32_t timeout_ms)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)received;
	(void)timeout_ms;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_sendto(ove_socket_t sock, const void *data, size_t len, size_t *sent,
				    const ove_sockaddr_t *dest)
{
	(void)sock;
	(void)data;
	(void)len;
	(void)sent;
	(void)dest;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len, size_t *received,
				      ove_sockaddr_t *src, uint32_t timeout_ms)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)received;
	(void)src;
	(void)timeout_ms;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint32_t timeout_ms)
{
	(void)hostname;
	(void)addr;
	(void)timeout_ms;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_sockaddr_ipv4(ove_sockaddr_t *addr, uint8_t a, uint8_t b, uint8_t c,
				     uint8_t d, uint16_t port)
{
	(void)addr;
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)port;
}
/** @endcond */

#endif /* CONFIG_OVE_NET */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_NET_H */
