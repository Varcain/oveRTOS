/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file net_httpd.hpp
 * @brief C++ wrappers for the oveRTOS embedded HTTP server API
 */

#pragma once

#include <ove/net_httpd.h>
#include <ove/types.hpp>
#include <ove/error.hpp>
#include <string_view>

#ifdef CONFIG_OVE_NET_HTTPD

namespace ove
{

/**
 * @namespace ove::httpd
 * @brief C++ wrappers around the oveRTOS embedded HTTP server API.
 *
 * Available when `CONFIG_OVE_NET_HTTPD` is enabled.  Provides thin RAII
 * wrappers around the request and response objects passed to route handlers,
 * plus free functions for server lifecycle and route registration.
 *
 * The server itself is a singleton managed by `start()` / `stop()`.
 */
namespace httpd
{

/**
 * @class Request
 * @brief Read-only view of an incoming HTTP request.
 *
 * Wraps the opaque `ove_httpd_req_t` pointer that is passed into route
 * handler callbacks.  Does not own the underlying storage; the pointer
 * remains valid for the duration of the handler invocation.
 *
 * @note Non-copyable and non-movable (lifetime is bound to the handler call).
 */
class Request
{
      public:
	/**
	 * @brief Constructs a Request view from a raw C request pointer.
	 * @param[in] raw Opaque request handle from the server callback.
	 */
	explicit Request(ove_httpd_req_t *raw) : raw_(raw)
	{
	}

	Request(const Request &) = delete;
	Request &operator=(const Request &) = delete;
	Request(Request &&) = delete;
	Request &operator=(Request &&) = delete;

	/** @brief Returns the HTTP method string ("GET", "POST", etc.). */
	const char *method() const
	{
		return ove_httpd_req_method(raw_);
	}

	/** @brief Returns the full request path (e.g. "/api/leds/0"). */
	const char *path() const
	{
		return ove_httpd_req_path(raw_);
	}

	/** @brief Returns the query string after '?' (or NULL). */
	const char *query() const
	{
		return ove_httpd_req_query(raw_);
	}

	/** @brief Returns the request body (or NULL). */
	const char *body() const
	{
		return ove_httpd_req_body(raw_);
	}

	/** @brief Returns the request body length in bytes. */
	size_t body_len() const
	{
		return ove_httpd_req_body_len(raw_);
	}

	/**
	 * @brief Returns a path segment by index.
	 *
	 * For path "/api/leds/0": segment(0)="api", segment(1)="leds",
	 * segment(2)="0".
	 *
	 * @param[in] idx Zero-based segment index.
	 * @return Segment string, or NULL if out of range.
	 */
	const char *segment(int idx) const
	{
		return ove_httpd_req_segment(raw_, idx);
	}

	/**
	 * @brief Returns the raw oveRTOS request pointer.
	 * @return The opaque `ove_httpd_req_t` pointer.
	 */
	ove_httpd_req_t *raw() const
	{
		return raw_;
	}

      private:
	ove_httpd_req_t *raw_;
};

/**
 * @class Response
 * @brief Helper for building and sending an HTTP response.
 *
 * Wraps the opaque `ove_httpd_resp_t` pointer that is passed into route
 * handler callbacks.  Does not own the underlying storage; the pointer
 * remains valid for the duration of the handler invocation.
 *
 * @note Non-copyable and non-movable (lifetime is bound to the handler call).
 */
class Response
{
      public:
	/**
	 * @brief Constructs a Response helper from a raw C response pointer.
	 * @param[in] raw Opaque response handle from the server callback.
	 */
	explicit Response(ove_httpd_resp_t *raw) : raw_(raw)
	{
	}

	Response(const Response &) = delete;
	Response &operator=(const Response &) = delete;
	Response(Response &&) = delete;
	Response &operator=(Response &&) = delete;

	/**
	 * @brief Sends a JSON response.
	 * @param[in] status HTTP status code (e.g. 200).
	 * @param[in] json   NUL-terminated JSON string.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	Result<void> json(int status, const char *json) noexcept
	{
		return from_rc(ove_httpd_resp_json(raw_, status, json));
	}

	/**
	 * @brief Sends an HTML response.
	 * @param[in] status HTTP status code.
	 * @param[in] html   HTML data.
	 * @param[in] len    HTML data length in bytes.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	Result<void> html(int status, const char *html, size_t len) noexcept
	{
		return from_rc(ove_httpd_resp_html(raw_, status, html, len));
	}

	/**
	 * @brief Sends a response with an arbitrary content type.
	 * @param[in] status       HTTP status code.
	 * @param[in] content_type Content-Type header value.
	 * @param[in] body         Response body.
	 * @param[in] len          Body length in bytes.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	Result<void> send(int status, const char *content_type, const void *body,
			  size_t len) noexcept
	{
		return from_rc(ove_httpd_resp_send(raw_, status, content_type, body, len));
	}

	/**
	 * @brief Sends a pre-gzipped response (adds Content-Encoding: gzip).
	 * @param[in] status       HTTP status code.
	 * @param[in] content_type Content-Type header value.
	 * @param[in] body         Gzip-compressed response body.
	 * @param[in] len          Compressed body length in bytes.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	Result<void> send_gz(int status, const char *content_type, const void *body,
			     size_t len) noexcept
	{
		return from_rc(ove_httpd_resp_send_gz(raw_, status, content_type, body, len));
	}

	/**
	 * @brief Sends a JSON error response.
	 * @param[in] status HTTP status code (e.g. 404, 500).
	 * @param[in] msg    Error message string.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	Result<void> error(int status, const char *msg) noexcept
	{
		return from_rc(ove_httpd_resp_error(raw_, status, msg));
	}

	/**
	 * @brief Returns the raw oveRTOS response pointer.
	 * @return The opaque `ove_httpd_resp_t` pointer.
	 */
	ove_httpd_resp_t *raw() const
	{
		return raw_;
	}

      private:
	ove_httpd_resp_t *raw_;
};

/** @brief Route handler callback type (same as the C typedef). */
using Handler = ove_httpd_handler_t;

/**
 * @struct Config
 * @brief HTTP server configuration.
 *
 * Mirrors `ove_httpd_config_t` with C++ default member initialisers.
 */
struct Config {
	uint16_t port{80};	 /**< Listen port. */
	int max_body_size{1024}; /**< Maximum POST body size in bytes. */
};

/**
 * @brief Starts the HTTP server.
 *
 * Spawns a background task that accepts connections on the configured port.
 * Routes may be registered before or after starting.
 *
 * @param[in] cfg Server configuration (defaults: port 80, 1024-byte body).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> start(const Config &cfg = {}) noexcept
{
	ove_httpd_config_t c{cfg.port, cfg.max_body_size};
	return from_rc(ove_httpd_start(&c));
}

/**
 * @brief Stops the HTTP server and closes the listening socket.
 */
inline void stop()
{
	ove_httpd_stop();
}

/**
 * @brief Registers a route handler.
 * @param[in] method  HTTP method string ("GET" or "POST").
 * @param[in] path    URL path prefix (e.g. "/api/leds").
 * @param[in] handler Callback function invoked when the route matches.
 * @return Empty `Result<void>` on success; `unexpected`
 *         @ref Error::NoMemory if the route table is full.
 */
[[nodiscard]] inline Result<void> route(const char *method, const char *path,
					Handler handler) noexcept
{
	return from_rc(ove_httpd_route(method, path, handler));
}

/**
 * @brief Registers the built-in dashboard routes (/api/info, /api/leds, etc.).
 *
 * Call after `start()` to add the standard device management API.
 */
inline void register_builtin_routes()
{
	ove_httpd_register_builtin_routes();
}

/**
 * @brief Associate a network interface with the dashboard routes.
 *
 * When set, /api/info and /api/network report the real interface
 * addresses instead of placeholders.
 */
inline void set_netif(ove_netif_t netif)
{
	ove_httpd_set_netif(netif);
}

#ifdef CONFIG_OVE_AUDIO
/** @brief Set the audio graph for /api/audio/stats. */
inline void set_audio_graph(struct ove_audio_graph *g)
{
	ove_httpd_set_audio_graph(g);
}
#endif

#ifdef CONFIG_OVE_INFER
/** @brief Set the ML model for /api/infer/stats. */
inline void set_model(ove_model_t model)
{
	ove_httpd_set_model(model);
}
#endif

} /* namespace httpd */

#ifdef CONFIG_OVE_NET_HTTPD_WS

/**
 * @namespace ove::ws
 * @brief C++ wrappers around the oveRTOS WebSocket API.
 *
 * Available when `CONFIG_OVE_NET_HTTPD_WS` is enabled.
 */
namespace ws
{

/**
 * @class Connection
 * @brief Non-owning handle to an active WebSocket connection.
 *
 * Valid only while the connection is active. Wraps the opaque
 * `ove_httpd_ws_conn_t` pointer.
 */
class Connection
{
      public:
	/** @brief Wraps an opaque `ove_httpd_ws_conn_t *` from the handler callback. */
	explicit Connection(ove_httpd_ws_conn_t *raw) : raw_(raw)
	{
	}

	/**
	 * @brief Send a text message to this connection.
	 * @param[in] data Message payload.
	 * @param[in] len  Payload length in bytes.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	Result<void> send(const void *data, size_t len) noexcept
	{
		return from_rc(ove_httpd_ws_send(raw_, data, len));
	}

	/**
	 * @brief Send a string_view as a text message.
	 * @param[in] sv String view to send.
	 * @return As @ref send(const void*, size_t).
	 */
	Result<void> send(std::string_view sv) noexcept
	{
		return from_rc(ove_httpd_ws_send(raw_, sv.data(), sv.size()));
	}

	/** @brief Returns the underlying `ove_httpd_ws_conn_t *` (for C-API escape hatches). */
	ove_httpd_ws_conn_t *raw() const
	{
		return raw_;
	}

      private:
	ove_httpd_ws_conn_t *raw_;
};

/** @brief WebSocket message handler type. */
using Handler = ove_httpd_ws_handler_t;

/** @brief WebSocket close handler type. */
using CloseHandler = ove_httpd_ws_close_handler_t;

/**
 * @brief Register a WebSocket route.
 * @param[in] path       URL path prefix (e.g. "/ws/log").
 * @param[in] on_message Callback for incoming messages.
 * @param[in] on_close   Callback when connection closes (may be nullptr).
 * @return Empty `Result<void>` on success; `unexpected`
 *         @ref Error::NoMemory if the route table is full.
 */
[[nodiscard]] inline Result<void> route(const char *path, Handler on_message,
					CloseHandler on_close = nullptr) noexcept
{
	return from_rc(ove_httpd_ws_route(path, on_message, on_close));
}

/**
 * @brief Broadcast a message to all WebSocket connections on a path.
 * @param[in] path Path filter (nullptr for all connections).
 * @param[in] data Message payload.
 * @param[in] len  Payload length in bytes.
 * @return Number of connections the message was sent to.
 */
inline int broadcast(const char *path, const void *data, size_t len)
{
	return ove_httpd_ws_broadcast(path, data, len);
}

/**
 * @brief Broadcast a string_view to all connections on a path.
 * @param[in] path Path filter (nullptr for all connections).
 * @param[in] sv   String view to send.
 * @return Number of connections the message was sent to.
 */
inline int broadcast(const char *path, std::string_view sv)
{
	return ove_httpd_ws_broadcast(path, sv.data(), sv.size());
}

/** @brief Return the number of active WebSocket connections. */
inline int active_count()
{
	return ove_httpd_ws_active_count();
}

} /* namespace ws */

#endif /* CONFIG_OVE_NET_HTTPD_WS */

} // namespace ove

#endif /* CONFIG_OVE_NET_HTTPD */
