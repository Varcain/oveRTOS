/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NET_HTTP_H
#define OVE_NET_HTTP_H

/**
 * @defgroup ove_net_http HTTP Client
 * @brief Portable HTTP/1.1 client for REST APIs.
 *
 * Supports GET and POST with optional TLS (when OVE_NET_TLS is enabled).
 * The client is portable C and delegates I/O to the socket/TLS layers.
 *
 * @note Requires @c CONFIG_OVE_NET_HTTP (implies @c CONFIG_OVE_NET).
 *       When disabled every function is replaced by a no-op stub.
 * @{
 */

#include "ove/types.h"
#include "ove/net.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief HTTP method. */
typedef enum {
	OVE_HTTP_GET    = 0, /**< HTTP GET. */
	OVE_HTTP_POST   = 1, /**< HTTP POST. */
	OVE_HTTP_PUT    = 2, /**< HTTP PUT. */
	OVE_HTTP_DELETE = 3, /**< HTTP DELETE. */
	OVE_HTTP_PATCH  = 4, /**< HTTP PATCH. */
} ove_http_method_t;

/**
 * @brief HTTP request header (name-value pair).
 */
typedef struct {
	const char *name;   /**< Header name (e.g. "Authorization"). */
	const char *value;  /**< Header value (e.g. "Bearer token"). */
} ove_http_header_t;

/**
 * @brief HTTP response (returned by request functions).
 */
typedef struct {
	int     status;       /**< HTTP status code (e.g. 200, 404). */
	char   *body;         /**< Response body (heap-allocated, NUL-terminated). */
	size_t  body_len;     /**< Body length in bytes (excluding NUL). */
	char   *headers;      /**< Raw response headers (heap-allocated). */
	size_t  headers_len;  /**< Headers length in bytes. */
} ove_http_response_t;

#include "ove/storage.h"

#ifdef CONFIG_OVE_NET_HTTP

/**
 * @brief Initialise an HTTP client from caller-supplied storage.
 *
 * @param[out] client  Handle written on success.
 * @param[in]  storage Caller-allocated storage.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_http_client_init(ove_http_client_t *client,
			      ove_http_client_storage_t *storage);

/**
 * @brief De-initialise an HTTP client.
 *
 * @param[in] client Handle returned by ove_http_client_init().
 */
void ove_http_client_deinit(ove_http_client_t client);

/**
 * @brief Perform an HTTP GET request.
 *
 * @param[in]  client HTTP client handle.
 * @param[in]  url    Full URL (e.g. "http://example.com/path").
 * @param[out] resp   Response filled on success.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_http_get(ove_http_client_t client, const char *url,
		      ove_http_response_t *resp);

/**
 * @brief Perform an HTTP POST request.
 *
 * @param[in]  client       HTTP client handle.
 * @param[in]  url          Full URL.
 * @param[in]  content_type Content-Type header value (e.g. "application/json").
 * @param[in]  body         Request body data.
 * @param[in]  body_len     Request body length in bytes.
 * @param[out] resp         Response filled on success.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_http_post(ove_http_client_t client, const char *url,
		       const char *content_type,
		       const void *body, size_t body_len,
		       ove_http_response_t *resp);

/**
 * @brief Perform a generic HTTP request.
 *
 * @param[in]  client       HTTP client handle.
 * @param[in]  method       HTTP method.
 * @param[in]  url          Full URL.
 * @param[in]  content_type Content-Type (may be NULL for GET).
 * @param[in]  body         Request body (may be NULL).
 * @param[in]  body_len     Request body length.
 * @param[out] resp         Response filled on success.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_http_request(ove_http_client_t client,
			  ove_http_method_t method, const char *url,
			  const char *content_type,
			  const void *body, size_t body_len,
			  ove_http_response_t *resp);

/**
 * @brief Perform an HTTP request with custom headers.
 *
 * @param[in]  client       HTTP client handle.
 * @param[in]  method       HTTP method (GET, POST, PUT, DELETE, PATCH).
 * @param[in]  url          Full URL.
 * @param[in]  content_type Content-Type (may be NULL).
 * @param[in]  body         Request body (may be NULL).
 * @param[in]  body_len     Request body length.
 * @param[in]  headers      Array of extra request headers (may be NULL).
 * @param[in]  header_count Number of headers in the array.
 * @param[out] resp         Response filled on success.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_http_request_ex(ove_http_client_t client,
			 ove_http_method_t method, const char *url,
			 const char *content_type,
			 const void *body, size_t body_len,
			 const ove_http_header_t *headers,
			 size_t header_count,
			 ove_http_response_t *resp);

/**
 * @brief Free resources in an HTTP response.
 *
 * @param[in] resp Response to free (body and headers are freed).
 */
void ove_http_response_free(ove_http_response_t *resp);

#ifdef OVE_HEAP_NET_HTTP
/**
 * @brief Heap-allocate and initialise an HTTP client.
 *
 * @param[out] client Handle written on success.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_http_client_create(ove_http_client_t *client);

/**
 * @brief Destroy a heap-allocated HTTP client.
 *
 * @param[in] client Handle returned by ove_http_client_create().
 */
void ove_http_client_destroy(ove_http_client_t client);
#endif /* OVE_HEAP_NET_HTTP */

#else /* !CONFIG_OVE_NET_HTTP */

/** @cond INTERNAL */
#ifndef CONFIG_OVE_NET_HTTP
typedef struct { uint8_t _unused; } ove_http_client_storage_t;
#endif

static inline int  ove_http_client_init(ove_http_client_t *client, ove_http_client_storage_t *storage) { (void)client; (void)storage; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_http_client_deinit(ove_http_client_t client) { (void)client; }
static inline int  ove_http_get(ove_http_client_t client, const char *url, ove_http_response_t *resp) { (void)client; (void)url; (void)resp; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_http_post(ove_http_client_t client, const char *url, const char *content_type, const void *body, size_t body_len, ove_http_response_t *resp) { (void)client; (void)url; (void)content_type; (void)body; (void)body_len; (void)resp; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_http_request(ove_http_client_t client, ove_http_method_t method, const char *url, const char *content_type, const void *body, size_t body_len, ove_http_response_t *resp) { (void)client; (void)method; (void)url; (void)content_type; (void)body; (void)body_len; (void)resp; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_http_request_ex(ove_http_client_t client, ove_http_method_t method, const char *url, const char *content_type, const void *body, size_t body_len, const ove_http_header_t *headers, size_t header_count, ove_http_response_t *resp) { (void)client; (void)method; (void)url; (void)content_type; (void)body; (void)body_len; (void)headers; (void)header_count; (void)resp; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_http_response_free(ove_http_response_t *resp) { (void)resp; }
/** @endcond */

#endif /* CONFIG_OVE_NET_HTTP */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_NET_HTTP_H */
