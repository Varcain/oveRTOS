/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Portable HTTP/1.1 client.
 *
 * Uses the oveRTOS socket layer (and optionally TLS) for transport.
 * Lives in backends/common/ because it has no platform-specific code.
 */

#include "ove/ove.h"
#include "ove/net_http.h"
#include "ove_backend_common.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------- URL parsing ---------- */

static int parse_url(const char *url, int *use_tls, char *host, size_t host_sz, uint16_t *port,
		     const char **path)
{
	*use_tls = 0;
	*port = 80;

	if (strncmp(url, "https://", 8) == 0) {
		*use_tls = 1;
		*port = 443;
		url += 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		url += 7;
	} else {
		return OVE_ERR_INVALID_PARAM;
	}

	/* Find host end (port or path) */
	const char *host_end = url;
	while (*host_end && *host_end != ':' && *host_end != '/')
		host_end++;

	size_t hlen = (size_t)(host_end - url);
	if (hlen == 0 || hlen >= host_sz)
		return OVE_ERR_INVALID_PARAM;
	memcpy(host, url, hlen);
	host[hlen] = '\0';

	if (*host_end == ':') {
		host_end++;
		char *endp = NULL;
		unsigned long p = strtoul(host_end, &endp, 10);
		if (endp == host_end || p == 0 || p > 65535)
			return OVE_ERR_INVALID_PARAM;
		*port = (uint16_t)p;
		host_end = endp;
	}

	*path = (*host_end == '/') ? host_end : "/";

	/* Bound path length so callers get a fast failure instead of a
	 * silently-truncated request line. Most real paths fit in 256. */
	if (strlen(*path) > 256)
		return OVE_ERR_INVALID_PARAM;

	return OVE_OK;
}

/* ---------- I/O helpers ---------- */

static int http_send_all(struct ove_http_client *c, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len > 0) {
		size_t sent = 0;
		int ret;
#ifdef CONFIG_OVE_NET_TLS
		if (c->use_tls && c->tls) {
			ret = ove_tls_send(c->tls, p, len, &sent);
		} else
#endif
		{
			ret = ove_socket_send(c->sock, p, len, &sent);
		}
		if (ret != OVE_OK)
			return ret;
		p += sent;
		len -= sent;
	}
	return OVE_OK;
}

static int http_recv_some(struct ove_http_client *c, void *buf, size_t len, size_t *received)
{
#ifdef CONFIG_OVE_NET_TLS
	if (c->use_tls && c->tls) {
		return ove_tls_recv(c->tls, buf, len, received);
	}
#endif
	return ove_socket_recv(c->sock, buf, len, received, OVE_WAIT_FOREVER);
}

/* ---------- HTTP client ---------- */

int ove_http_client_init(ove_http_client_t *client, ove_http_client_storage_t *storage)
{
	if (!client || !storage)
		return OVE_ERR_INVALID_PARAM;
	struct ove_http_client *c = (struct ove_http_client *)storage;
	memset(c, 0, sizeof(*c));
	*client = c;
	return OVE_OK;
}

void ove_http_client_deinit(ove_http_client_t client)
{
	if (client) {
		memset(client, 0, sizeof(*client));
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_http_client_create(ove_http_client_t *client)
{
	if (!client)
		return OVE_ERR_INVALID_PARAM;
	struct ove_http_client *c = OVE_BACKEND_MALLOC(sizeof(*c));
	if (!c)
		return OVE_ERR_NO_MEMORY;
	memset(c, 0, sizeof(*c));
	*client = c;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_http_client_destroy(ove_http_client_t client)
{
	if (client) {
		memset(client, 0, sizeof(*client));
		OVE_BACKEND_FREE(client);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_http_get(ove_http_client_t client, const char *url, ove_http_response_t *resp)
{
	return ove_http_request(client, OVE_HTTP_GET, url, NULL, NULL, 0, resp);
}

int ove_http_post(ove_http_client_t client, const char *url, const char *content_type,
		  const void *body, size_t body_len, ove_http_response_t *resp)
{
	return ove_http_request(client, OVE_HTTP_POST, url, content_type, body, body_len, resp);
}

int ove_http_request(ove_http_client_t client, ove_http_method_t method, const char *url,
		     const char *content_type, const void *body, size_t body_len,
		     ove_http_response_t *resp)
{
	return ove_http_request_ex(client, method, url, content_type, body, body_len, NULL, 0,
				   resp);
}

int ove_http_request_ex(ove_http_client_t client, ove_http_method_t method, const char *url,
			const char *content_type, const void *body, size_t body_len,
			const ove_http_header_t *headers, size_t header_count,
			ove_http_response_t *resp)
{
	if (!client || !url || !resp)
		return OVE_ERR_INVALID_PARAM;
	struct ove_http_client *c = client;

	memset(resp, 0, sizeof(*resp));

	/* Parse URL */
	int use_tls = 0;
	uint16_t port = 80;
	const char *path = "/";
	int ret = parse_url(url, &use_tls, c->host, sizeof(c->host), &port, &path);
	if (ret != OVE_OK)
		return ret;
	c->port = port;
	c->use_tls = use_tls;

	/* DNS resolve */
	ove_sockaddr_t addr;
	ret = ove_dns_resolve(c->host, &addr, OVE_MS(10000));
	if (ret != OVE_OK)
		return ret;
	addr.port = port;

	/* Open socket */
	ove_socket_storage_t sock_storage;
	ret = ove_socket_open(&c->sock, &sock_storage, OVE_AF_INET, OVE_SOCK_STREAM);
	if (ret != OVE_OK)
		return ret;

	/* Connect */
	ret = ove_socket_connect(c->sock, &addr, OVE_MS(10000));
	if (ret != OVE_OK)
		goto cleanup_sock;

		/* TLS handshake if HTTPS */
#ifdef CONFIG_OVE_NET_TLS
	ove_tls_storage_t tls_storage;
	ove_tls_t tls = NULL;
	if (use_tls) {
		ret = ove_tls_init(&tls, &tls_storage);
		if (ret != OVE_OK)
			goto cleanup_sock;
		/* TODO: expose CA cert / mTLS knobs on the HTTP client so
		 * callers can configure proper verification. Today the
		 * client has no config surface, so we explicitly opt into
		 * unverified TLS and emit a warning (see net_tls.h). */
		ove_tls_config_t tls_cfg = {
			.ca_cert = NULL,
			.ca_cert_len = 0,
			.hostname = c->host,
			.allow_insecure = 1,
		};
		ret = ove_tls_handshake(tls, c->sock, &tls_cfg);
		if (ret != OVE_OK) {
			ove_tls_deinit(tls);
			goto cleanup_sock;
		}
		c->tls = tls;
	}
#else
	if (use_tls) {
		ret = OVE_ERR_NOT_SUPPORTED;
		goto cleanup_sock;
	}
#endif

	/* Build request */
	const char *method_str;
	switch (method) {
	case OVE_HTTP_POST:
		method_str = "POST";
		break;
	case OVE_HTTP_PUT:
		method_str = "PUT";
		break;
	case OVE_HTTP_DELETE:
		method_str = "DELETE";
		break;
	case OVE_HTTP_PATCH:
		method_str = "PATCH";
		break;
	default:
		method_str = "GET";
		break;
	}
	char req_line[512];
	int hlen = snprintf(req_line, sizeof(req_line),
			    "%s %s HTTP/1.1\r\n"
			    "Host: %s\r\n"
			    "Connection: close\r\n",
			    method_str, path, c->host);
	if (hlen < 0 || (size_t)hlen >= sizeof(req_line)) {
		ret = OVE_ERR_INVALID_PARAM;
		goto cleanup_tls;
	}

	ret = http_send_all(c, req_line, (size_t)hlen);
	if (ret != OVE_OK)
		goto cleanup_tls;

	/* Send custom headers */
	for (size_t hi = 0; hi < header_count; hi++) {
		if (!headers[hi].name || !headers[hi].value)
			continue;
		char hdr_line[256];
		int hn = snprintf(hdr_line, sizeof(hdr_line), "%s: %s\r\n", headers[hi].name,
				  headers[hi].value);
		if (hn < 0 || (size_t)hn >= sizeof(hdr_line)) {
			/* Header too long for the line buffer — fail rather than
			 * silently omit it (and never feed an out-of-range length
			 * to http_send_all). */
			ret = OVE_ERR_INVALID_PARAM;
			goto cleanup_tls;
		}
		ret = http_send_all(c, hdr_line, (size_t)hn);
		if (ret != OVE_OK)
			goto cleanup_tls;
	}

	if (content_type && body) {
		char ct_hdr[256];
		int n = snprintf(ct_hdr, sizeof(ct_hdr),
				 "Content-Type: %s\r\n"
				 "Content-Length: %zu\r\n",
				 content_type, body_len);
		if (n < 0 || (size_t)n >= sizeof(ct_hdr)) {
			/* Truncated/error: (size_t)n would over-read past ct_hdr
			 * (a long content_type, or n == -1 -> SIZE_MAX). */
			ret = OVE_ERR_INVALID_PARAM;
			goto cleanup_tls;
		}
		ret = http_send_all(c, ct_hdr, (size_t)n);
		if (ret != OVE_OK)
			goto cleanup_tls;
	}

	ret = http_send_all(c, "\r\n", 2);
	if (ret != OVE_OK)
		goto cleanup_tls;

	if (body && body_len > 0) {
		ret = http_send_all(c, body, body_len);
		if (ret != OVE_OK)
			goto cleanup_tls;
	}

	/* Read response */
#ifdef CONFIG_OVE_ZERO_HEAP
	size_t cap = sizeof(c->_resp_buf);
	char *buf = c->_resp_buf;
#else
	size_t cap = 4096;
	char *buf = OVE_BACKEND_MALLOC(cap);
	if (!buf) {
		ret = OVE_ERR_NO_MEMORY;
		goto cleanup_tls;
	}
#endif
	size_t total = 0;

	for (;;) {
#ifndef CONFIG_OVE_ZERO_HEAP
		if (total + 1024 > cap) {
			cap *= 2;
			char *nb = OVE_BACKEND_MALLOC(cap);
			if (!nb) {
				OVE_BACKEND_FREE(buf);
				ret = OVE_ERR_NO_MEMORY;
				goto cleanup_tls;
			}
			memcpy(nb, buf, total);
			OVE_BACKEND_FREE(buf);
			buf = nb;
		}
#else
		if (total + 1 >= cap) {
			ret = OVE_ERR_NO_MEMORY;
			goto cleanup_tls;
		}
#endif
		size_t got = 0;
		ret = http_recv_some(c, buf + total, cap - total - 1, &got);
		if (ret == OVE_ERR_NET_CLOSED)
			break;
		if (ret != OVE_OK) {
#ifndef CONFIG_OVE_ZERO_HEAP
			OVE_BACKEND_FREE(buf);
#endif
			goto cleanup_tls;
		}
		total += got;
	}
	buf[total] = '\0';

	/* Parse status line */
	char *status_end = strstr(buf, "\r\n");
	if (status_end) {
		char *sp = strchr(buf, ' ');
		if (sp)
			resp->status = atoi(sp + 1);
	}

	/* Split headers and body */
	char *body_start = strstr(buf, "\r\n\r\n");
	if (body_start) {
		size_t hdr_len = (size_t)(body_start - buf);
		body_start += 4;
		size_t blen = total - (size_t)(body_start - buf);

#ifdef CONFIG_OVE_ZERO_HEAP
		/* Borrowed pointers into client buffer — valid until next request */
		buf[hdr_len] = '\0';
		resp->headers = buf;
		resp->headers_len = hdr_len;
		resp->body = body_start;
		resp->body_len = blen;
#else
		resp->headers = OVE_BACKEND_MALLOC(hdr_len + 1);
		if (!resp->headers) {
			OVE_BACKEND_FREE(buf);
			ret = OVE_ERR_NO_MEMORY;
			goto cleanup_tls;
		}
		memcpy(resp->headers, buf, hdr_len);
		resp->headers[hdr_len] = '\0';
		resp->headers_len = hdr_len;

		resp->body = OVE_BACKEND_MALLOC(blen + 1);
		if (!resp->body) {
			OVE_BACKEND_FREE(resp->headers);
			resp->headers = NULL;
			resp->headers_len = 0;
			OVE_BACKEND_FREE(buf);
			ret = OVE_ERR_NO_MEMORY;
			goto cleanup_tls;
		}
		memcpy(resp->body, body_start, blen);
		resp->body[blen] = '\0';
		resp->body_len = blen;
#endif
	}

#ifndef CONFIG_OVE_ZERO_HEAP
	OVE_BACKEND_FREE(buf);
#endif
	ret = OVE_OK;

cleanup_tls:
#ifdef CONFIG_OVE_NET_TLS
	if (use_tls && tls) {
		ove_tls_close(tls);
		ove_tls_deinit(tls);
		c->tls = NULL;
	}
#endif

cleanup_sock:
	ove_socket_close(c->sock);
	c->sock = NULL;
	return ret;
}

void ove_http_response_free(ove_http_response_t *resp)
{
	if (!resp)
		return;
#ifndef CONFIG_OVE_ZERO_HEAP
	if (resp->body) {
		OVE_BACKEND_FREE(resp->body);
	}
	if (resp->headers) {
		OVE_BACKEND_FREE(resp->headers);
	}
#endif
	resp->body = NULL;
	resp->headers = NULL;
	resp->body_len = 0;
	resp->headers_len = 0;
	resp->status = 0;
}
