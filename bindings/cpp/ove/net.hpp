/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file net.hpp
 * @brief RAII networking: sockets, network interface, DNS
 */

#pragma once

#include <ove/net.h>
#include <ove/types.hpp>

#ifdef CONFIG_OVE_NET

namespace ove
{

/* ── Address (value type, copyable) ─────────────────────────────── */

/**
 * @class Address
 * @brief Lightweight wrapper around `ove_sockaddr_t`.
 *
 * A copyable value type that holds an IP address and port.  Use the
 * `ipv4()` factory to construct from individual octets, or access the
 * `raw` member directly for advanced use.
 */
class Address
{
      public:
	/**
	 * @brief Default-constructs a zeroed address.
	 */
	Address() : raw{}
	{
	}

	/**
	 * @brief Creates an IPv4 address from individual octets and a port.
	 * @param[in] a    First octet.
	 * @param[in] b    Second octet.
	 * @param[in] c    Third octet.
	 * @param[in] d    Fourth octet.
	 * @param[in] port Port number in host byte order.
	 * @return A fully initialised `Address`.
	 */
	static Address ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port)
	{
		Address addr;
		ove_sockaddr_ipv4(&addr.raw, a, b, c, d, port);
		return addr;
	}

	/**
	 * @brief Returns the port in host byte order.
	 * @return Port number.
	 */
	uint16_t port() const
	{
		return raw.port;
	}

	/**
	 * @brief Sets the port.
	 * @param[in] p Port number in host byte order.
	 */
	void set_port(uint16_t p)
	{
		raw.port = p;
	}

	/** @brief The underlying C sockaddr. */
	ove_sockaddr_t raw;
};

/* ── NetIfConfig (builder, copyable) ────────────────────────────── */

/**
 * @class NetIfConfig
 * @brief Builder for `ove_netif_config_t`.
 *
 * Wraps the C configuration struct and provides a fluent builder API.
 * The `raw` member can be passed directly to the C API if needed.
 */
class NetIfConfig
{
      public:
	/**
	 * @brief Default-constructs a zeroed configuration.
	 */
	NetIfConfig() : raw{}
	{
	}

	/**
	 * @brief Configures a static IP address.
	 * @param[in] ip   Static IP address.
	 * @param[in] mask Subnet mask.
	 * @param[in] gw   Default gateway.
	 * @return Reference to this object for chaining.
	 */
	NetIfConfig &static_ip(const Address &ip, const Address &mask, const Address &gw)
	{
		raw.use_dhcp = 0;
		raw.static_ip = ip.raw;
		raw.netmask = mask.raw;
		raw.gateway = gw.raw;
		return *this;
	}

	/**
	 * @brief Enables DHCP.
	 * @return Reference to this object for chaining.
	 */
	NetIfConfig &dhcp()
	{
		raw.use_dhcp = 1;
		return *this;
	}

	/**
	 * @brief Sets the DNS server address.
	 * @param[in] d DNS server address.
	 * @return Reference to this object for chaining.
	 */
	NetIfConfig &dns(const Address &d)
	{
		raw.dns = d.raw;
		return *this;
	}

	/** @brief The underlying C configuration struct. */
	ove_netif_config_t raw;
};

/* ── NetIf (RAII network interface) ─────────────────────────────── */

/**
 * @class NetIf
 * @brief RAII wrapper around an oveRTOS network interface.
 *
 * The interface is initialised on construction and de-initialised on
 * destruction.  Call `up()` with a configuration to activate the link
 * and `down()` to tear it down.
 *
 * @note Non-copyable.  Move-only when heap allocation is enabled.
 */
class NetIf
{
      public:
	/**
	 * @brief Constructs and initialises the network interface.
	 *
	 * Asserts at startup if initialisation fails.
	 */
	NetIf()
	{
		int err = ove_netif_init(&handle_, &storage_);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief De-initialises the network interface.
	 */
	~NetIf()
	{
		if (!handle_)
			return;
		ove_netif_deinit(handle_);
	}

	NetIf(const NetIf &) = delete;
	NetIf &operator=(const NetIf &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	NetIf(NetIf &&) = delete;
	NetIf &operator=(NetIf &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	NetIf(NetIf &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	NetIf &operator=(NetIf &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_netif_deinit(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Brings the network interface up.
	 * @param[in] cfg Interface configuration (DHCP or static).
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int up(const NetIfConfig &cfg)
	{
		return ove_netif_up(handle_, &cfg.raw);
	}

	/**
	 * @brief Tears down the network interface.
	 */
	void down()
	{
		ove_netif_down(handle_);
	}

	/**
	 * @brief Query the current addresses of the network interface.
	 * @param[out] ip      Current IP address (may be nullptr).
	 * @param[out] gateway Current gateway (may be nullptr).
	 * @param[out] netmask Current subnet mask (may be nullptr).
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int get_addr(Address *ip = nullptr, Address *gateway = nullptr,
				   Address *netmask = nullptr)
	{
		return ove_netif_get_addr(handle_, ip ? &ip->raw : nullptr,
					  gateway ? &gateway->raw : nullptr,
					  netmask ? &netmask->raw : nullptr);
	}

	/**
	 * @brief Returns `true` if the underlying handle is non-null.
	 * @return `true` when the interface was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS network interface handle.
	 * @return The opaque `ove_netif_t` handle.
	 */
	ove_netif_t handle() const
	{
		return handle_;
	}

      private:
	ove_netif_t handle_{};
	ove_netif_storage_t storage_{};
};

/* ── Forward declaration for friend access ──────────────────────── */

class TcpListener;

/* ── TcpSocket (RAII TCP socket) ────────────────────────────────── */

/**
 * @class TcpSocket
 * @brief RAII wrapper around an oveRTOS TCP (stream) socket.
 *
 * A new socket is opened on construction and closed on destruction.
 * `TcpListener::accept()` can also produce a `TcpSocket` by adopting
 * an already-accepted connection via the private constructor.
 *
 * @note Non-copyable.  Move-only when heap allocation is enabled.
 */
class TcpSocket
{
      public:
	/**
	 * @brief Opens a new TCP socket.
	 *
	 * Asserts at startup if socket creation fails.
	 */
	TcpSocket()
	{
		int err = ove_socket_open(&handle_, &storage_, OVE_AF_INET, OVE_SOCK_STREAM);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
		open_ = true;
	}

	/**
	 * @brief Closes the socket if it is still open.
	 */
	~TcpSocket()
	{
		if (open_)
			ove_socket_close(handle_);
	}

	TcpSocket(const TcpSocket &) = delete;
	TcpSocket &operator=(const TcpSocket &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	TcpSocket(TcpSocket &&) = delete;
	TcpSocket &operator=(TcpSocket &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the socket.
	 * @param other The source; left in a closed, null state after the move.
	 */
	TcpSocket(TcpSocket &&other) noexcept
		: handle_(other.handle_), storage_(other.storage_), open_(other.open_)
	{
		other.handle_ = nullptr;
		other.open_ = false;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the socket.
	 * @param other The source; left in a closed, null state after the move.
	 * @return Reference to this object.
	 */
	TcpSocket &operator=(TcpSocket &&other) noexcept
	{
		if (this != &other) {
			if (open_)
				ove_socket_close(handle_);
			handle_ = other.handle_;
			storage_ = other.storage_;
			open_ = other.open_;
			other.handle_ = nullptr;
			other.open_ = false;
		}
		return *this;
	}
#endif

	/**
	 * @brief Connects to a remote address.
	 * @param[in] addr       Remote address.
	 * @param[in] timeout_ms Timeout in milliseconds (`OVE_WAIT_FOREVER` to block).
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int connect(const Address &addr, uint32_t timeout_ms = OVE_WAIT_FOREVER)
	{
		return ove_socket_connect(handle_, &addr.raw, timeout_ms);
	}

	/**
	 * @brief Sends data on the connected socket.
	 * @param[in]  data Pointer to data to send.
	 * @param[in]  len  Number of bytes to send.
	 * @param[out] sent Number of bytes actually sent (may be nullptr).
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int send(const void *data, size_t len, size_t *sent = nullptr)
	{
		return ove_socket_send(handle_, data, len, sent);
	}

	/**
	 * @brief Receives data from the connected socket.
	 * @param[out] buf        Buffer to receive into.
	 * @param[in]  len        Buffer size in bytes.
	 * @param[out] received   Number of bytes received (may be nullptr).
	 * @param[in]  timeout_ms Timeout in milliseconds (`OVE_WAIT_FOREVER` to block).
	 * @return `OVE_OK` on success, `OVE_ERR_NET_CLOSED` if peer closed.
	 */
	[[nodiscard]] int recv(void *buf, size_t len, size_t *received = nullptr,
			       uint32_t timeout_ms = OVE_WAIT_FOREVER)
	{
		return ove_socket_recv(handle_, buf, len, received, timeout_ms);
	}

	/**
	 * @brief Closes the socket.
	 *
	 * Safe to call on an already-closed socket.
	 */
	void close()
	{
		if (open_) {
			ove_socket_close(handle_);
			open_ = false;
		}
	}

	/**
	 * @brief Returns `true` if the socket is open.
	 * @return Socket open state.
	 */
	bool is_open() const
	{
		return open_;
	}

	/**
	 * @brief Returns the raw oveRTOS socket handle.
	 * @return The opaque `ove_socket_t` handle.
	 */
	ove_socket_t handle() const
	{
		return handle_;
	}

      private:
	friend class TcpListener;

	/**
	 * @brief Adopts an already-accepted connection (used by TcpListener).
	 * @param[in] h Socket handle from ove_socket_accept().
	 * @param[in] s Storage backing the accepted socket.
	 */
	TcpSocket(ove_socket_t h, ove_socket_storage_t s) : handle_(h), storage_(s), open_(true)
	{
	}

	ove_socket_t handle_{};
	ove_socket_storage_t storage_{};
	bool open_{false};
};

/* ── UdpSocket (RAII UDP socket) ────────────────────────────────── */

/**
 * @class UdpSocket
 * @brief RAII wrapper around an oveRTOS UDP (datagram) socket.
 *
 * A new socket is opened on construction and closed on destruction.
 *
 * @note Non-copyable.  Move-only when heap allocation is enabled.
 */
class UdpSocket
{
      public:
	/**
	 * @brief Opens a new UDP socket.
	 *
	 * Asserts at startup if socket creation fails.
	 */
	UdpSocket()
	{
		int err = ove_socket_open(&handle_, &storage_, OVE_AF_INET, OVE_SOCK_DGRAM);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
		open_ = true;
	}

	/**
	 * @brief Closes the socket if it is still open.
	 */
	~UdpSocket()
	{
		if (open_)
			ove_socket_close(handle_);
	}

	UdpSocket(const UdpSocket &) = delete;
	UdpSocket &operator=(const UdpSocket &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	UdpSocket(UdpSocket &&) = delete;
	UdpSocket &operator=(UdpSocket &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the socket.
	 * @param other The source; left in a closed, null state after the move.
	 */
	UdpSocket(UdpSocket &&other) noexcept
		: handle_(other.handle_), storage_(other.storage_), open_(other.open_)
	{
		other.handle_ = nullptr;
		other.open_ = false;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the socket.
	 * @param other The source; left in a closed, null state after the move.
	 * @return Reference to this object.
	 */
	UdpSocket &operator=(UdpSocket &&other) noexcept
	{
		if (this != &other) {
			if (open_)
				ove_socket_close(handle_);
			handle_ = other.handle_;
			storage_ = other.storage_;
			open_ = other.open_;
			other.handle_ = nullptr;
			other.open_ = false;
		}
		return *this;
	}
#endif

	/**
	 * @brief Binds the socket to a local address.
	 * @param[in] addr Local address to bind.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int bind(const Address &addr)
	{
		return ove_socket_bind(handle_, &addr.raw);
	}

	/**
	 * @brief Sends a datagram to a specific destination.
	 * @param[in]  data Pointer to data to send.
	 * @param[in]  len  Number of bytes to send.
	 * @param[in]  dest Destination address.
	 * @param[out] sent Number of bytes actually sent (may be nullptr).
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int send_to(const void *data, size_t len, const Address &dest,
				  size_t *sent = nullptr)
	{
		return ove_socket_sendto(handle_, data, len, sent, &dest.raw);
	}

	/**
	 * @brief Receives a datagram and the sender's address.
	 * @param[out] buf        Buffer to receive into.
	 * @param[in]  len        Buffer size in bytes.
	 * @param[out] src        Filled with sender's address (may be nullptr).
	 * @param[out] received   Number of bytes received (may be nullptr).
	 * @param[in]  timeout_ms Timeout in milliseconds (`OVE_WAIT_FOREVER` to block).
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int recv_from(void *buf, size_t len, Address *src = nullptr,
				    size_t *received = nullptr,
				    uint32_t timeout_ms = OVE_WAIT_FOREVER)
	{
		return ove_socket_recvfrom(handle_, buf, len, received, src ? &src->raw : nullptr,
					   timeout_ms);
	}

	/**
	 * @brief Closes the socket.
	 *
	 * Safe to call on an already-closed socket.
	 */
	void close()
	{
		if (open_) {
			ove_socket_close(handle_);
			open_ = false;
		}
	}

	/**
	 * @brief Returns `true` if the socket is open.
	 * @return Socket open state.
	 */
	bool is_open() const
	{
		return open_;
	}

	/**
	 * @brief Returns the raw oveRTOS socket handle.
	 * @return The opaque `ove_socket_t` handle.
	 */
	ove_socket_t handle() const
	{
		return handle_;
	}

      private:
	ove_socket_t handle_{};
	ove_socket_storage_t storage_{};
	bool open_{false};
};

/* ── TcpListener (RAII listening socket) ────────────────────────── */

/**
 * @class TcpListener
 * @brief RAII wrapper around an oveRTOS TCP listening socket.
 *
 * Opens a stream socket on construction, then `bind()` + `listen()` to
 * begin accepting connections.  `accept()` returns a `TcpSocket` that
 * owns the accepted connection.
 *
 * @note Non-copyable.  Move-only when heap allocation is enabled.
 */
class TcpListener
{
      public:
	/**
	 * @brief Opens a new TCP socket for listening.
	 *
	 * Asserts at startup if socket creation fails.
	 */
	TcpListener()
	{
		int err = ove_socket_open(&handle_, &storage_, OVE_AF_INET, OVE_SOCK_STREAM);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
		open_ = true;
	}

	/**
	 * @brief Closes the listening socket if it is still open.
	 */
	~TcpListener()
	{
		if (open_)
			ove_socket_close(handle_);
	}

	TcpListener(const TcpListener &) = delete;
	TcpListener &operator=(const TcpListener &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	TcpListener(TcpListener &&) = delete;
	TcpListener &operator=(TcpListener &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the socket.
	 * @param other The source; left in a closed, null state after the move.
	 */
	TcpListener(TcpListener &&other) noexcept
		: handle_(other.handle_), storage_(other.storage_), open_(other.open_)
	{
		other.handle_ = nullptr;
		other.open_ = false;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the socket.
	 * @param other The source; left in a closed, null state after the move.
	 * @return Reference to this object.
	 */
	TcpListener &operator=(TcpListener &&other) noexcept
	{
		if (this != &other) {
			if (open_)
				ove_socket_close(handle_);
			handle_ = other.handle_;
			storage_ = other.storage_;
			open_ = other.open_;
			other.handle_ = nullptr;
			other.open_ = false;
		}
		return *this;
	}
#endif

	/**
	 * @brief Binds the socket to a local address.
	 * @param[in] addr Local address to bind.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int bind(const Address &addr)
	{
		return ove_socket_bind(handle_, &addr.raw);
	}

	/**
	 * @brief Marks the socket as listening for incoming connections.
	 * @param[in] backlog Maximum pending connection queue length.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int listen(int backlog = 4)
	{
		return ove_socket_listen(handle_, backlog);
	}

	/**
	 * @brief Accepts an incoming connection.
	 *
	 * On success the returned `TcpSocket` owns the accepted connection.
	 * The caller must check the return value; on failure the output
	 * `client` is left in a closed state.
	 *
	 * @param[out] client     Receives the accepted connection.
	 * @param[in]  timeout_ms Timeout in milliseconds (`OVE_WAIT_FOREVER` to block).
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int accept(TcpSocket &client, uint32_t timeout_ms = OVE_WAIT_FOREVER)
	{
		ove_socket_t cli_handle{};
		ove_socket_storage_t cli_storage{};
		int err = ove_socket_accept(handle_, &cli_handle, &cli_storage, timeout_ms);
		if (err == OVE_OK) {
			client.close();
			client.handle_ = cli_handle;
			client.storage_ = cli_storage;
			client.open_ = true;
		}
		return err;
	}

	/**
	 * @brief Closes the listening socket.
	 *
	 * Safe to call on an already-closed socket.
	 */
	void close()
	{
		if (open_) {
			ove_socket_close(handle_);
			open_ = false;
		}
	}

	/**
	 * @brief Returns `true` if the listening socket is open.
	 * @return Socket open state.
	 */
	bool is_open() const
	{
		return open_;
	}

	/**
	 * @brief Returns the raw oveRTOS socket handle.
	 * @return The opaque `ove_socket_t` handle.
	 */
	ove_socket_t handle() const
	{
		return handle_;
	}

      private:
	ove_socket_t handle_{};
	ove_socket_storage_t storage_{};
	bool open_{false};
};

/* ── DNS ────────────────────────────────────────────────────────── */

/**
 * @namespace ove::dns
 * @brief DNS resolution helpers.
 *
 * Available when `CONFIG_OVE_NET` is enabled.
 */
namespace dns
{

/**
 * @brief Resolves a hostname to an address.
 * @param[in]  hostname   Null-terminated hostname string.
 * @param[out] addr       Receives the resolved address.
 * @param[in]  timeout_ms Timeout in milliseconds.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int resolve(const char *hostname, Address &addr, uint32_t timeout_ms = 5000)
{
	return ove_dns_resolve(hostname, &addr.raw, timeout_ms);
}

} /* namespace dns */

} /* namespace ove */

#endif /* CONFIG_OVE_NET */
