/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file net_tls.hpp
 * @brief RAII wrapper for TLS/SSL sessions (mbedTLS)
 */

#pragma once

#include <ove/net_tls.h>
#include <ove/net.hpp>
#include <ove/types.hpp>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_NET_TLS

namespace ove::tls
{

/**
 * @brief TLS session configuration.
 */
struct Config {
	const unsigned char *ca_cert{};	    /**< PEM/DER CA certificate (nullptr to skip). */
	size_t ca_cert_len{};		    /**< Certificate length (incl NUL for PEM). */
	const char *hostname{};		    /**< SNI hostname (may be nullptr). */
	const unsigned char *client_cert{}; /**< PEM/DER client cert for mTLS (nullptr to skip). */
	size_t client_cert_len{};	    /**< Client certificate length. */
	const unsigned char *client_key{};  /**< PEM/DER client private key (nullptr to skip). */
	size_t client_key_len{};	    /**< Client key length. */
};

/**
 * @class Session
 * @brief RAII wrapper around an oveRTOS TLS session.
 *
 * Initialises mbedTLS contexts on construction, frees them on destruction.
 * Use handshake() to establish a TLS connection over an existing TCP socket.
 */
class Session
{
      public:
	Session()
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_tls_init(&handle_, &storage_);
#else
		int err = ove_tls_create(&handle_);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	~Session() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_tls_deinit(handle_);
#else
		ove_tls_destroy(handle_);
#endif
	}

	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;
	Session(Session &&) = delete;
	Session &operator=(Session &&) = delete;

	/**
	 * @brief Perform TLS handshake over an established TCP socket.
	 *
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure (cert validation, network error,
	 *         etc.).
	 */
	[[nodiscard]] Result<void> handshake(ove_socket_t sock, const Config &cfg = {}) noexcept
	{
		ove_tls_config_t c{cfg.ca_cert,	      cfg.ca_cert_len,	   cfg.hostname,
				   cfg.client_cert,   cfg.client_cert_len, cfg.client_key,
				   cfg.client_key_len};
		return from_rc(ove_tls_handshake(handle_, sock, &c));
	}

	/**
	 * @brief Send encrypted bytes over the TLS session.
	 *
	 * @param[in] data Buffer to send.
	 * @param[in] len  Buffer length in bytes.
	 * @return On success, the number of bytes actually sent.  On
	 *         failure, an `unexpected` @ref Error.
	 */
	[[nodiscard]] Result<size_t> send(const void *data, size_t len) noexcept
	{
		size_t sent = 0;
		const int rc = ove_tls_send(handle_, data, len, &sent);
		return from_rc(rc, sent);
	}

	/**
	 * @brief Receive decrypted bytes from the TLS session.
	 *
	 * @param[out] buf Destination buffer.
	 * @param[in]  len Buffer capacity in bytes.
	 * @return On success, the number of bytes received.  On failure,
	 *         an `unexpected` @ref Error (`Error::NetClosed` for a
	 *         clean TLS shutdown).
	 */
	[[nodiscard]] Result<size_t> recv(void *buf, size_t len) noexcept
	{
		size_t received = 0;
		const int rc = ove_tls_recv(handle_, buf, len, &received);
		return from_rc(rc, received);
	}

	/** @brief Close the TLS session (sends close_notify and tears down state). */
	void close()
	{
		ove_tls_close(handle_);
	}

      private:
	ove_tls_t handle_{};
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_tls_storage_t storage_{};
#endif
};

} // namespace ove::tls

#endif /* CONFIG_OVE_NET_TLS */
