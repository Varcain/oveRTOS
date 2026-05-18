/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file net_http.hpp
 * @brief RAII C++ wrappers for the oveRTOS HTTP client API
 */

#pragma once

#include <ove/net_http.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_NET_HTTP

namespace ove::http
{

/**
 * @namespace ove::http
 * @brief C++ wrappers around the oveRTOS HTTP client API.
 *
 * Available when `CONFIG_OVE_NET_HTTP` is enabled.  Provides a RAII `Client`
 * that manages the lifecycle of the underlying C client handle, and a
 * move-only `Response` that owns the response body and headers.
 */

/**
 * @class Response
 * @brief RAII wrapper around an oveRTOS HTTP response.
 *
 * Owns the body and header buffers returned by an HTTP request.  Frees
 * them automatically on destruction.
 *
 * @note Non-copyable; movable.  The moved-from object is left in a
 *       zeroed (empty) state.
 */
class Response
{
      public:
	/**
	 * @brief Constructs an empty response.
	 */
	Response() = default;

	/**
	 * @brief Destroys the response, freeing body and header buffers.
	 */
	~Response() noexcept
	{
		ove_http_response_free(&raw_);
	}

	Response(const Response &) = delete;
	Response &operator=(const Response &) = delete;

	/**
	 * @brief Move constructor -- transfers ownership of response buffers.
	 * @param other The source; its raw response is zeroed after the move.
	 */
	Response(Response &&other) noexcept : raw_(other.raw_)
	{
		other.raw_ = {};
	}

	/**
	 * @brief Move-assignment operator -- frees current buffers and takes ownership.
	 * @param other The source; its raw response is zeroed after the move.
	 * @return Reference to this object.
	 */
	Response &operator=(Response &&other) noexcept
	{
		if (this != &other) {
			ove_http_response_free(&raw_);
			raw_ = other.raw_;
			other.raw_ = {};
		}
		return *this;
	}

	/** @brief Returns the HTTP status code (e.g. 200, 404). */
	int status() const
	{
		return raw_.status;
	}

	/** @brief Returns the response body (NUL-terminated). */
	const char *body() const
	{
		return raw_.body;
	}

	/** @brief Returns the response body length in bytes. */
	size_t body_len() const
	{
		return raw_.body_len;
	}

	/** @brief Returns the raw response headers. */
	const char *headers() const
	{
		return raw_.headers;
	}

	/** @brief Returns the response headers length in bytes. */
	size_t headers_len() const
	{
		return raw_.headers_len;
	}

	/**
	 * @brief Returns a pointer to the underlying C response struct.
	 * @return Pointer to the raw `ove_http_response_t`.
	 */
	ove_http_response_t *raw()
	{
		return &raw_;
	}

	ove_http_response_t raw_{}; /**< Underlying C response struct (populated by the client). */
};

/**
 * @class Client
 * @brief RAII wrapper around an oveRTOS HTTP client handle.
 *
 * Constructs the underlying HTTP client on creation and destroys it on
 * destruction.  With `CONFIG_OVE_ZERO_HEAP` the client storage is held
 * inline in the wrapper; move operations are therefore disabled in that
 * configuration because the kernel may hold a pointer to the internal buffer.
 *
 * @note Non-copyable.  Move-only when heap allocation is enabled.
 */
class Client
{
      public:
	/**
	 * @brief Constructs and initialises the HTTP client.
	 *
	 * Calls `ove_http_client_init` (zero-heap) or `ove_http_client_create`
	 * (heap).  Asserts at startup if initialisation fails.
	 */
	Client()
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_http_client_init(&handle_, &storage_);
#else
		int err = ove_http_client_create(&handle_);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the HTTP client, releasing the underlying resource.
	 *
	 * If the handle is null (e.g., after a move), the destructor is a no-op.
	 */
	~Client() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_http_client_deinit(handle_);
#else
		ove_http_client_destroy(handle_);
#endif
	}

	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Client(Client &&) = delete;
	Client &operator=(Client &&) = delete;
#else
	/**
	 * @brief Move constructor -- transfers ownership of the client handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Client(Client &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator -- transfers ownership of the client handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Client &operator=(Client &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_http_client_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Performs an HTTP GET request.
	 * @param[in] url Full URL (e.g. "http://example.com/path").
	 * @return On success, the populated @ref Response (owns the body
	 *         and header buffers).  On failure, an `unexpected`
	 *         @ref Error.
	 */
	[[nodiscard]] Result<Response> get(const char *url) noexcept
	{
		Response resp;
		const int rc = ove_http_get(handle_, url, &resp.raw_);
		return from_rc(rc, std::move(resp));
	}

	/**
	 * @brief Performs an HTTP POST request.
	 * @param[in] url          Full URL.
	 * @param[in] content_type Content-Type header value.
	 * @param[in] body         Request body data.
	 * @param[in] body_len     Request body length in bytes.
	 * @return On success, the populated @ref Response.  On failure,
	 *         an `unexpected` @ref Error.
	 */
	[[nodiscard]] Result<Response> post(const char *url, const char *content_type,
					    const void *body, size_t body_len) noexcept
	{
		Response resp;
		const int rc =
			ove_http_post(handle_, url, content_type, body, body_len, &resp.raw_);
		return from_rc(rc, std::move(resp));
	}

	/**
	 * @brief Performs a generic HTTP request.
	 * @param[in] method       HTTP method (GET, POST, …).
	 * @param[in] url          Full URL.
	 * @param[in] content_type Content-Type (may be NULL for GET).
	 * @param[in] body         Request body (may be NULL).
	 * @param[in] body_len     Request body length.
	 * @return On success, the populated @ref Response.  On failure,
	 *         an `unexpected` @ref Error.
	 */
	[[nodiscard]] Result<Response> request(ove_http_method_t method, const char *url,
					       const char *content_type, const void *body,
					       size_t body_len) noexcept
	{
		Response resp;
		const int rc = ove_http_request(handle_, method, url, content_type, body, body_len,
						&resp.raw_);
		return from_rc(rc, std::move(resp));
	}

	/**
	 * @brief Performs an HTTP request with custom headers.
	 * @param[in] method       HTTP method.
	 * @param[in] url          Full URL.
	 * @param[in] content_type Content-Type (may be NULL).
	 * @param[in] body         Request body (may be NULL).
	 * @param[in] body_len     Request body length.
	 * @param[in] headers      Array of extra request headers.
	 * @param[in] header_count Number of headers.
	 * @return On success, the populated @ref Response.  On failure,
	 *         an `unexpected` @ref Error.
	 */
	[[nodiscard]] Result<Response>
	request(ove_http_method_t method, const char *url, const char *content_type,
		const void *body, size_t body_len, const ove_http_header_t *headers,
		size_t header_count) noexcept
	{
		Response resp;
		const int rc = ove_http_request_ex(handle_, method, url, content_type, body,
						   body_len, headers, header_count, &resp.raw_);
		return from_rc(rc, std::move(resp));
	}

	/**
	 * @brief Returns `true` if the underlying client handle is non-null.
	 * @return `true` when the client was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS HTTP client handle.
	 * @return The opaque `ove_http_client_t` handle.
	 */
	ove_http_client_t handle() const
	{
		return handle_;
	}

      private:
	ove_http_client_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_http_client_storage_t storage_ = {};
#endif
};

} /* namespace ove::http */

#endif /* CONFIG_OVE_NET_HTTP */
