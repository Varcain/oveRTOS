/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NET_HTTPD_H
#define OVE_NET_HTTPD_H

/**
 * @file net_httpd.h
 * @defgroup ove_net_httpd HTTP Server
 * @brief Lightweight embedded HTTP server with REST API routing.
 *
 * Provides a single-threaded HTTP/1.1 server with path-based route
 * registration, JSON response helpers, and an optional built-in
 * web dashboard for device management.
 *
 * @note Requires @c CONFIG_OVE_NET_HTTPD (implies @c CONFIG_OVE_NET).
 * @{
 */

#include "ove/types.h"
#include "ove/net.h"
#include "ove_config.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_NET_HTTPD

/** @brief Maximum number of registered routes. */
#define OVE_HTTPD_MAX_ROUTES 16

/** @brief Maximum path segments for parsing (e.g. /api/leds/0 = 3). */
#define OVE_HTTPD_MAX_SEGMENTS 8

/** @brief Opaque HTTP request. */
typedef struct ove_httpd_req ove_httpd_req_t;

/** @brief Opaque HTTP response. */
typedef struct ove_httpd_resp ove_httpd_resp_t;

/**
 * @brief Route handler callback.
 *
 * Runs on the server's single @c httpd thread (8 KiB stack, OVE_PRIO_NORMAL),
 * one request at a time, so a slow handler delays every client.  Must not
 * call ove_httpd_stop(), which waits for that thread.
 *
 * @return OVE_OK on success (response sent), negative error code on failure.
 */
typedef int (*ove_httpd_handler_t)(ove_httpd_req_t *req, ove_httpd_resp_t *resp);

/**
 * @brief HTTP server configuration.
 */
typedef struct {
	uint16_t port;	   /**< Listen port (default 80). */
	int max_body_size; /**< Max POST body bytes (default 1024). */
} ove_httpd_config_t;

/**
 * @brief Start the HTTP server.
 *
 * Spawns a background task that accepts connections on the configured port.
 * Register routes with ove_httpd_route() before starting: the route tables
 * are not locked against the server thread, so a route added while it runs
 * is not guaranteed to be seen consistently.
 *
 * @param[in] cfg Server configuration (NULL for defaults).
 * @return OVE_OK on success, @c OVE_ERR_BUSY if already running, or another
 *         negative error code on failure.
 */
OVE_NODISCARD int ove_httpd_start(const ove_httpd_config_t *cfg);

/**
 * @brief Stop the HTTP server and close the listening socket.
 *
 * Waits for the server thread to exit, so no handler is running once this
 * returns; never call it from a handler.  On its way out the server thread
 * sends each open WebSocket connection a close frame (1001, going away),
 * calls its @c on_close and closes it.
 */
void ove_httpd_stop(void);

/**
 * @brief Register a route handler.
 *
 * The @p method and @p path strings are copied (truncated to the table's
 * field sizes); only @p handler is kept.
 *
 * @param[in] method  HTTP method ("GET" or "POST").
 * @param[in] path    URL path prefix (e.g. "/api/leds").
 * @param[in] handler Callback function.
 * @return OVE_OK on success, OVE_ERR_NO_MEMORY if route table full.
 */
OVE_NODISCARD int ove_httpd_route(const char *method, const char *path,
				  ove_httpd_handler_t handler);

/**
 * @brief Register built-in dashboard routes (/api/info, /api/leds, etc.).
 *
 * Adds the standard device management API; like any route, best registered
 * before ove_httpd_start() (see there).
 */
void ove_httpd_register_builtin_routes(void);

/**
 * @brief Set the network interface used by built-in dashboard routes.
 *
 * When set, /api/info and /api/network return the real interface
 * addresses instead of placeholder values.
 *
 * @param[in] netif Network interface handle.
 */
void ove_httpd_set_netif(ove_netif_t netif);

/* ── Optional module hooks ──────────────────────────────────── */

#ifdef CONFIG_OVE_AUDIO
struct ove_audio_graph;
/** @brief Set the audio graph for /api/audio/stats. */
void ove_httpd_set_audio_graph(struct ove_audio_graph *g);
#endif

#ifdef CONFIG_OVE_INFER
/** @brief Set the ML model for /api/infer/stats. */
void ove_httpd_set_model(ove_model_t model);
#endif

/* ── Request accessors ───────────────────────────────────────── */

/** @brief Get the HTTP method string ("GET" or "POST"). */
const char *ove_httpd_req_method(ove_httpd_req_t *req);

/** @brief Get the full request path (e.g. "/api/leds/0"). */
const char *ove_httpd_req_path(ove_httpd_req_t *req);

/** @brief Get the query string after '?' (or NULL). */
const char *ove_httpd_req_query(ove_httpd_req_t *req);

/** @brief Get the request body (or NULL). */
const char *ove_httpd_req_body(ove_httpd_req_t *req);

/** @brief Get the request body length. */
size_t ove_httpd_req_body_len(ove_httpd_req_t *req);

/**
 * @brief Get a path segment by index.
 *
 * For path "/api/leds/0": segment 0="api", 1="leds", 2="0".
 *
 * @param[in] req Request.
 * @param[in] idx Segment index (0-based).
 * @return Segment string or NULL if out of range.
 */
const char *ove_httpd_req_segment(ove_httpd_req_t *req, int idx);

/* ── Response helpers ────────────────────────────────────────── */

/** @brief Send a JSON response. */
OVE_NODISCARD int ove_httpd_resp_json(ove_httpd_resp_t *resp, int status, const char *json);

/** @brief Send an HTML response. */
OVE_NODISCARD int ove_httpd_resp_html(ove_httpd_resp_t *resp, int status, const char *html,
				      size_t len);

/** @brief Send a response with arbitrary content type. */
OVE_NODISCARD int ove_httpd_resp_send(ove_httpd_resp_t *resp, int status, const char *content_type,
				      const void *body, size_t len);

/** @brief Send a pre-gzipped response (adds Content-Encoding: gzip). */
OVE_NODISCARD int ove_httpd_resp_send_gz(ove_httpd_resp_t *resp, int status,
					 const char *content_type, const void *body, size_t len);

/** @brief Send a JSON error response. */
OVE_NODISCARD int ove_httpd_resp_error(ove_httpd_resp_t *resp, int status, const char *message);

/* ── WebSocket support ──────────────────────────────────────── */

#ifdef CONFIG_OVE_NET_HTTPD_WS

/** @brief Opaque WebSocket connection handle. */
typedef struct ove_httpd_ws_conn ove_httpd_ws_conn_t;

/**
 * @brief WebSocket message callback.
 *
 * Runs on the server's @c httpd thread, like the route handlers.
 * @param[in] conn Connection that sent the message.
 * @param[in] data Message payload (text or binary).
 * @param[in] len  Payload length in bytes.
 */
typedef void (*ove_httpd_ws_handler_t)(ove_httpd_ws_conn_t *conn, const void *data, size_t len);

/**
 * @brief WebSocket close callback.
 *
 * Runs on the server's @c httpd thread once for every connection that
 * closes: when the client closes it or breaks the protocol, on the poll
 * after a send to it fails, and at ove_httpd_stop().  The @p conn handle
 * stays valid until this returns; afterwards its slot can go to a new
 * client, so drop every copy of it here.
 *
 * @param[in] conn Connection that was closed.
 */
typedef void (*ove_httpd_ws_close_handler_t)(ove_httpd_ws_conn_t *conn);

/**
 * @brief Register a WebSocket route.
 *
 * When a client sends an HTTP upgrade request matching @p path, the
 * connection is upgraded to WebSocket.  Incoming messages are
 * dispatched to @p on_message.  The @p path string is copied.
 *
 * @param[in] path       URL path prefix (e.g. "/ws/log").
 * @param[in] on_message Callback for incoming messages (may be NULL).
 * @param[in] on_close   Callback when connection closes (may be NULL).
 * @return OVE_OK on success, OVE_ERR_NO_MEMORY if route table full.
 */
OVE_NODISCARD int ove_httpd_ws_route(const char *path, ove_httpd_ws_handler_t on_message,
				     ove_httpd_ws_close_handler_t on_close);

/**
 * @brief Send a text message to a WebSocket connection.
 *
 * Any thread may call it; frames from different threads never interleave.
 * If the send fails the connection is closing: later sends to it fail, and
 * its @c on_close runs on the @c httpd thread shortly after, which is when
 * @p conn stops being valid.  A frame the client does not take within a
 * second fails the send, and every other sender waits for it meanwhile.
 *
 * @param[in] conn Target connection.
 * @param[in] data Message payload.
 * @param[in] len  Payload length in bytes.
 * @return OVE_OK on success, OVE_ERR_NET_CLOSED if the connection is closing,
 *         or another negative error code if the send failed.
 */
OVE_NODISCARD int ove_httpd_ws_send(ove_httpd_ws_conn_t *conn, const void *data, size_t len);

/**
 * @brief Broadcast a text message to all WebSocket connections on a path.
 *
 * Any thread may call it.  A connection whose send fails is closing, as
 * for ove_httpd_ws_send().
 *
 * @param[in] path Path filter (NULL for all connections).
 * @param[in] data Message payload.
 * @param[in] len  Payload length in bytes.
 * @return Number of connections the message was sent to.
 */
int ove_httpd_ws_broadcast(const char *path, const void *data, size_t len);

/** @brief Return the number of active WebSocket connections. */
int ove_httpd_ws_active_count(void);

/* ── Internal — called by httpd accept loop ──────────────────────────── */

/** @brief Return non-zero if the HTTP headers request a WebSocket upgrade. */
int ove_httpd_ws_is_upgrade(const char *headers);

/**
 * @brief Complete the WebSocket upgrade handshake on `sock`.
 *
 * Invoked by the httpd accept loop after `ove_httpd_ws_is_upgrade()` returns
 * true.  On success the connection is handed off to the WS subsystem.
 */
OVE_NODISCARD int ove_httpd_ws_handshake(const char *headers, size_t headers_len, const char *path,
					 ove_socket_t sock, ove_socket_storage_t *storage);

/** @brief Drive the WS subsystem from the httpd task's poll loop. */
void ove_httpd_ws_poll(void);

#endif /* CONFIG_OVE_NET_HTTPD_WS */

/* ── Log hook ────────────────────────────────────────────────── */

/**
 * @brief Feed a log line into the httpd log ring buffer.
 *
 * Call from the log output hook to capture lines for GET /api/log.
 */
void ove_httpd_log_append(const char *line);

#else /* !CONFIG_OVE_NET_HTTPD */

/** @cond INTERNAL */
OVE_NODISCARD static inline int ove_httpd_start(const void *cfg)
{
	(void)cfg;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_httpd_stop(void)
{
}
static inline void ove_httpd_register_builtin_routes(void)
{
}
static inline void ove_httpd_log_append(const char *line)
{
	(void)line;
}
/** @endcond */

#endif /* CONFIG_OVE_NET_HTTPD */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_NET_HTTPD_H */
