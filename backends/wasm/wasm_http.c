/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM HTTP client — backed by emscripten_fetch().
 *
 * Makes real HTTPS requests via the browser's Fetch API.  The browser
 * handles TLS, DNS, cookies, CORS, etc.
 *
 * This replaces the common ove_net_http.c (which uses sockets+TLS)
 * when building for Emscripten.  The API is identical.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_NET_HTTP) && defined(__EMSCRIPTEN__)

#include "ove/net_http.h"
#include "ove_backend_common.h"

#include <emscripten/fetch.h>
#include <stdlib.h>
#include <string.h>

/* ── Client lifecycle (trivial — no persistent state needed) ───────── */

int ove_http_client_init(ove_http_client_t *client,
			 ove_http_client_storage_t *storage)
{
	if (!client || !storage) return OVE_ERR_INVALID_PARAM;
	struct ove_http_client *c = (struct ove_http_client *)storage;
	memset(c, 0, sizeof(*c));
	*client = c;
	return OVE_OK;
}

void ove_http_client_deinit(ove_http_client_t client)
{
	(void)client;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_http_client_create(ove_http_client_t *client)
{
	if (!client) return OVE_ERR_INVALID_PARAM;
	struct ove_http_client *c = OVE_BACKEND_MALLOC(sizeof(*c));
	if (!c) return OVE_ERR_NO_MEMORY;
	memset(c, 0, sizeof(*c));
	*client = c;
	return OVE_OK;
}

void ove_http_client_destroy(ove_http_client_t client)
{
	if (client) OVE_BACKEND_FREE(client);
}
#endif

/* ── Request execution via emscripten_fetch ────────────────────────── */

static const char *method_str(ove_http_method_t m)
{
	switch (m) {
	case OVE_HTTP_GET:    return "GET";
	case OVE_HTTP_POST:   return "POST";
	case OVE_HTTP_PUT:    return "PUT";
	case OVE_HTTP_DELETE: return "DELETE";
	case OVE_HTTP_PATCH:  return "PATCH";
	default:              return "GET";
	}
}

int ove_http_request_ex(ove_http_client_t client,
			ove_http_method_t method, const char *url,
			const char *content_type,
			const void *body, size_t body_len,
			const ove_http_header_t *headers,
			size_t header_count,
			ove_http_response_t *resp)
{
	(void)client;
	if (!url || !resp) return OVE_ERR_INVALID_PARAM;
	memset(resp, 0, sizeof(*resp));

	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	strncpy(attr.requestMethod, method_str(method),
		sizeof(attr.requestMethod) - 1);
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY
			| EMSCRIPTEN_FETCH_SYNCHRONOUS;

	/* Request body. */
	if (body && body_len > 0) {
		attr.requestData = (const char *)body;
		attr.requestDataSize = body_len;
	}

	/* Headers: content_type + custom headers.
	 * emscripten_fetch wants a NULL-terminated array of
	 * [name, value, name, value, ..., NULL]. */
	size_t total_hdrs = header_count + (content_type ? 1 : 0);
	const char **hdr_array = NULL;
	if (total_hdrs > 0) {
		hdr_array = calloc(total_hdrs * 2 + 1, sizeof(char *));
		if (hdr_array) {
			size_t idx = 0;
			if (content_type) {
				hdr_array[idx++] = "Content-Type";
				hdr_array[idx++] = content_type;
			}
			for (size_t i = 0; i < header_count; i++) {
				hdr_array[idx++] = headers[i].name;
				hdr_array[idx++] = headers[i].value;
			}
			hdr_array[idx] = NULL;
			attr.requestHeaders = hdr_array;
		}
	}

	emscripten_fetch_t *fetch = emscripten_fetch(&attr, url);
	free(hdr_array);

	if (!fetch)
		return OVE_ERR_NET_UNREACHABLE;

	resp->status = (int)fetch->status;

	/* Copy response body. */
	if (fetch->numBytes > 0) {
		resp->body = malloc(fetch->numBytes + 1);
		if (resp->body) {
			memcpy(resp->body, fetch->data, fetch->numBytes);
			resp->body[fetch->numBytes] = '\0';
			resp->body_len = fetch->numBytes;
		}
	}

	/* Response headers not easily accessible from fetch API;
	 * leave as NULL for now. */
	resp->headers = NULL;
	resp->headers_len = 0;

	emscripten_fetch_close(fetch);

	if (resp->status == 0)
		return OVE_ERR_NET_UNREACHABLE;
	return OVE_OK;
}

int ove_http_request(ove_http_client_t client,
		     ove_http_method_t method, const char *url,
		     const char *content_type,
		     const void *body, size_t body_len,
		     ove_http_response_t *resp)
{
	return ove_http_request_ex(client, method, url, content_type,
				   body, body_len, NULL, 0, resp);
}

int ove_http_get(ove_http_client_t client, const char *url,
		 ove_http_response_t *resp)
{
	return ove_http_request(client, OVE_HTTP_GET, url, NULL, NULL, 0, resp);
}

int ove_http_post(ove_http_client_t client, const char *url,
		  const char *content_type,
		  const void *body, size_t body_len,
		  ove_http_response_t *resp)
{
	return ove_http_request(client, OVE_HTTP_POST, url, content_type,
				body, body_len, resp);
}

void ove_http_response_free(ove_http_response_t *resp)
{
	if (!resp) return;
	free(resp->body);
	free(resp->headers);
	memset(resp, 0, sizeof(*resp));
}

#endif /* CONFIG_OVE_NET_HTTP && __EMSCRIPTEN__ */
