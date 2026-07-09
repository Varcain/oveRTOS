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
 * @file net.h
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
#define OVE_SOCK_RAW ((ove_sock_type_t)3)
typedef uint8_t ove_af_t;
#define OVE_AF_INET ((ove_af_t)2)
#define OVE_AF_INET6 ((ove_af_t)10)
#else /* @endcond */
typedef enum {
	OVE_SOCK_STREAM = 1, /**< Reliable byte-stream (TCP). */
	OVE_SOCK_DGRAM = 2,  /**< Connectionless datagrams (UDP). */
	OVE_SOCK_RAW = 3,    /**< Raw IP protocol access (e.g. ICMP for ping). */
} ove_sock_type_t;

/** @brief Address family. */
typedef enum {
	OVE_AF_INET = 2,   /**< IPv4. */
	OVE_AF_INET6 = 10, /**< IPv6. */
} ove_af_t;
#endif

/** @brief Socket readiness bits for @ref ove_socket_poll (Linux poll(2) values). */
#define OVE_SOCK_POLLIN 0x01u  /**< Readable: data ready, or an incoming connection. */
#define OVE_SOCK_POLLOUT 0x04u /**< Writable: send won't block, or connect completed. */
#define OVE_SOCK_POLLERR 0x08u /**< Error condition (also reports a failed connect). */
#define OVE_SOCK_POLLHUP 0x10u /**< Peer hung up. */

/** @brief Shutdown directions for @ref ove_socket_shutdown (POSIX values). */
#define OVE_SHUT_RD 0	/**< Further receives disallowed. */
#define OVE_SHUT_WR 1	/**< Further sends disallowed. */
#define OVE_SHUT_RDWR 2 /**< Both disallowed. */

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

/** ove_netif status flags (ove_netif_get_flags). The Linux personality maps these
 *  to the guest's IFF_* values; keep them engine-neutral here. */
#define OVE_NETIF_FLAG_UP 0x01u	       /**< Administratively up. */
#define OVE_NETIF_FLAG_BROADCAST 0x02u /**< Broadcast capable. */
#define OVE_NETIF_FLAG_LOOPBACK 0x04u  /**< Loopback interface. */
#define OVE_NETIF_FLAG_RUNNING 0x08u   /**< Link/carrier up. */
#define OVE_NETIF_FLAG_MULTICAST 0x10u /**< Multicast capable. */

/**
 * @brief Reconfigure the interface's address(es). A NULL field is left unchanged.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_netif_set_addr(ove_netif_t netif, const ove_sockaddr_t *ip, const ove_sockaddr_t *netmask,
		       const ove_sockaddr_t *gateway);

/** @brief Bring the interface administratively up (up != 0) or down (up == 0). */
int ove_netif_set_up(ove_netif_t netif, int up);

/** @brief Copy the interface's 6-byte hardware (MAC) address into @p mac. */
int ove_netif_get_hwaddr(ove_netif_t netif, uint8_t mac[6]);

/** @brief Read the interface's OVE_NETIF_FLAG_* bitmask into @p flags. */
int ove_netif_get_flags(ove_netif_t netif, unsigned *flags);

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
 * @brief Open a socket with an explicit IP protocol number.
 *
 * Needed for SOCK_RAW (e.g. proto == 1 / IPPROTO_ICMP for ping). @p proto == 0
 * selects the type's default protocol, so this is equivalent to ove_socket_open
 * for SOCK_STREAM / SOCK_DGRAM.
 *
 * @param[out] sock    Handle written on success.
 * @param[in]  storage Caller-allocated storage.
 * @param[in]  af      Address family.
 * @param[in]  type    Socket type (stream/datagram/raw).
 * @param[in]  proto   IP protocol number (0 = default for @p type).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_open_ex(ove_socket_t *sock, ove_socket_storage_t *storage, ove_af_t af,
		       ove_sock_type_t type, int proto);

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
 * @param[in] timeout_ns Timeout in nanoseconds (OVE_WAIT_FOREVER to block).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint64_t timeout_ns);

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
 * @param[in]  timeout_ns     Timeout in nanoseconds.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_accept(ove_socket_t sock, ove_socket_t *client, ove_socket_storage_t *client_storage,
		      uint64_t timeout_ns);

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
 * @param[in]  timeout_ns Timeout in nanoseconds.
 * @return OVE_OK on success, OVE_ERR_NET_CLOSED if peer closed.
 */
int ove_socket_recv(ove_socket_t sock, void *buf, size_t len, size_t *received,
		    uint64_t timeout_ns);

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
 * @param[in]  timeout_ns Timeout in nanoseconds.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len, size_t *received,
			ove_sockaddr_t *src, uint64_t timeout_ns);

/**
 * @brief Enable or disable non-blocking mode on a socket.
 *
 * With non-blocking mode on, the blocking socket calls return
 * @c OVE_ERR_TIMEOUT immediately instead of waiting when they would block.
 * This is the safe primitive for a caller that drives its own readiness loop
 * (the Linux personality's park/retry coordinator): unlike a zero
 * @c timeout_ns — which some backends map to @c SO_RCVTIMEO and interpret as
 * "block forever" — this reliably makes every operation return at once.
 *
 * @param[in] sock     Socket handle.
 * @param[in] nonblock Non-zero to enable non-blocking mode, zero to clear it.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_set_nonblock(ove_socket_t sock, int nonblock);

/**
 * @brief Wait for readiness on a socket (select/poll with a timeout).
 *
 * The only readiness wait that behaves uniformly across backends: pass
 * @c timeout_ns == 0 for an immediate, truly non-blocking poll.
 *
 * @param[in]  sock       Socket handle.
 * @param[in]  events     Requested @c OVE_SOCK_POLL* bits.
 * @param[out] revents    Ready @c OVE_SOCK_POLL* bits (may be NULL).
 * @param[in]  timeout_ns Timeout in nanoseconds (0 = poll, OVE_WAIT_FOREVER = block).
 * @return OVE_OK on success (inspect @p revents), negative error code on failure.
 */
int ove_socket_poll(ove_socket_t sock, unsigned events, unsigned *revents, uint64_t timeout_ns);

/**
 * @brief Shut down part or all of a full-duplex connection.
 *
 * @param[in] sock Socket handle.
 * @param[in] how  @c OVE_SHUT_RD, @c OVE_SHUT_WR, or @c OVE_SHUT_RDWR.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_shutdown(ove_socket_t sock, int how);

/**
 * @brief Get the local address a socket is bound to.
 *
 * @param[in]  sock Socket handle.
 * @param[out] addr Filled with the local address.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_getsockname(ove_socket_t sock, ove_sockaddr_t *addr);

/**
 * @brief Get the remote address a socket is connected to.
 *
 * @param[in]  sock Socket handle.
 * @param[out] addr Filled with the peer address.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_socket_getpeername(ove_socket_t sock, ove_sockaddr_t *addr);

/**
 * @brief Read and clear a socket's pending error (@c SO_ERROR).
 *
 * Used to obtain the result of a non-blocking connect once the socket
 * reports writable via @ref ove_socket_poll.
 *
 * @param[in] sock Socket handle.
 * @return OVE_OK if no error is pending, otherwise a negative @c OVE_ERR_NET_*.
 */
int ove_socket_get_error(ove_socket_t sock);

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
 * @param[in]  timeout_ns Timeout in nanoseconds.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint64_t timeout_ns);

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
static inline int ove_netif_set_addr(ove_netif_t netif, const ove_sockaddr_t *ip,
				     const ove_sockaddr_t *nm, const ove_sockaddr_t *gw)
{
	(void)netif;
	(void)ip;
	(void)nm;
	(void)gw;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_netif_set_up(ove_netif_t netif, int up)
{
	(void)netif;
	(void)up;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_netif_get_hwaddr(ove_netif_t netif, uint8_t mac[6])
{
	(void)netif;
	(void)mac;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_netif_get_flags(ove_netif_t netif, unsigned *flags)
{
	(void)netif;
	(void)flags;
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
static inline int ove_socket_open_ex(ove_socket_t *sock, ove_socket_storage_t *storage, ove_af_t af,
				     ove_sock_type_t type, int proto)
{
	(void)sock;
	(void)storage;
	(void)af;
	(void)type;
	(void)proto;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_socket_close(ove_socket_t sock)
{
	(void)sock;
}
static inline int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr,
				     uint64_t timeout_ns)
{
	(void)sock;
	(void)addr;
	(void)timeout_ns;
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
				    ove_socket_storage_t *client_storage, uint64_t timeout_ns)
{
	(void)sock;
	(void)client;
	(void)client_storage;
	(void)timeout_ns;
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
				  uint64_t timeout_ns)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)received;
	(void)timeout_ns;
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
static inline int ove_socket_set_nonblock(ove_socket_t sock, int nonblock)
{
	(void)sock;
	(void)nonblock;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_poll(ove_socket_t sock, unsigned events, unsigned *revents,
				  uint64_t timeout_ns)
{
	(void)sock;
	(void)events;
	(void)revents;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_shutdown(ove_socket_t sock, int how)
{
	(void)sock;
	(void)how;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_getsockname(ove_socket_t sock, ove_sockaddr_t *addr)
{
	(void)sock;
	(void)addr;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_getpeername(ove_socket_t sock, ove_sockaddr_t *addr)
{
	(void)sock;
	(void)addr;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_socket_get_error(ove_socket_t sock)
{
	(void)sock;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr, uint64_t timeout_ns)
{
	(void)hostname;
	(void)addr;
	(void)timeout_ns;
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
