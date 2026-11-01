/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Portable single-threaded HTTP server.
 *
 * Uses the oveRTOS socket API for transport and spawns a background
 * thread to accept connections.  Supports GET/POST routing with path
 * prefix matching.  Lives in backends/common/ because it has no
 * platform-specific code.
 */

#include "ove/ove.h"
#include "ove/net_httpd.h"
#include "ove_backend_common.h"
#include "ove_net_httpd_ws_internal.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Escape a complete counted JSON string, reserving one trailing NUL. */
static int json_escape_n(const char *in, size_t len, char *out, size_t cap)
{
	size_t used = 0;
	if (!cap || (!in && len) || (len && memchr(in, 0, len)))
		return OVE_ERR_INVALID_PARAM;
	for (size_t i = 0; i < len; ++i) {
		unsigned char c = (unsigned char)in[i];
		const char *part = &in[i];
		size_t count = 1;
		char escaped[7];
		switch (c) {
		case '"':
			part = "\\\"";
			count = 2;
			break;
		case '\\':
			part = "\\\\";
			count = 2;
			break;
		case '\n':
			part = "\\n";
			count = 2;
			break;
		case '\r':
			part = "\\r";
			count = 2;
			break;
		case '\t':
			part = "\\t";
			count = 2;
			break;
		case '\b':
			part = "\\b";
			count = 2;
			break;
		case '\f':
			part = "\\f";
			count = 2;
			break;
		default:
			if (c < 0x20) {
				(void)snprintf(escaped, sizeof(escaped), "\\u%04x", c);
				part = escaped;
				count = 6;
			}
			break;
		}
		if (count > cap - 1 - used)
			return OVE_ERR_NO_SPACE;
		memcpy(out + used, part, count);
		used += count;
	}
	out[used] = 0;
	return OVE_OK;
}

/* ---------- Internal data structures ---------- */

/* Routes may be added while the server runs: a registrant claims a slot
 * by advancing s_route_count, fills it and then sets ready, and the server
 * skips a slot until it is ready.  A slot never changes once ready. */
struct ove_httpd_route {
	char method[OVE_HTTPD_METHOD_SIZE];
	char path[OVE_HTTPD_ROUTE_SIZE];
	ove_httpd_handler_t handler;
	int ready;
};

struct ove_httpd_req {
	char method[8];
	char path[256];	   /* preserved for route matching */
	char seg_buf[256]; /* mutable copy for segment splitting */
	char *query;
	char *body;
	size_t body_len;
	char *segments[OVE_HTTPD_MAX_SEGMENTS];
	int segment_count;
};

struct ove_httpd_resp {
	ove_socket_t sock;
	int sent; /* flag: response already sent */
};

/* ---------- Route table ---------- */

static struct ove_httpd_route s_routes[OVE_HTTPD_MAX_ROUTES];
static int s_route_count;

/* ---------- Server state ---------- */

static ove_socket_t s_server_sock;
static ove_socket_storage_t s_server_storage;
static uint16_t s_port;

/* Thread for the server loop */
static ove_thread_t s_thread;

/* ---------- Status code helper ---------- */

static const char *status_str(int code)
{
	switch (code) {
	case 200:
		return "200 OK";
	case 400:
		return "400 Bad Request";
	case 404:
		return "404 Not Found";
	case 500:
		return "500 Internal Server Error";
	default:
		return "200 OK";
	}
}

/* ---------- Route registration ---------- */

int ove_httpd_route(const char *method, const char *path, ove_httpd_handler_t handler)
{
	if (!method || !path || !handler)
		return OVE_ERR_INVALID_PARAM;
	if (strnlen(method, OVE_HTTPD_METHOD_SIZE) >= OVE_HTTPD_METHOD_SIZE ||
	    strnlen(path, OVE_HTTPD_ROUTE_SIZE) >= OVE_HTTPD_ROUTE_SIZE)
		return OVE_ERR_NAME_TOO_LONG;

	int idx = __atomic_load_n(&s_route_count, __ATOMIC_RELAXED);
	do {
		if (idx >= OVE_HTTPD_MAX_ROUTES)
			return OVE_ERR_NO_MEMORY;
	} while (!__atomic_compare_exchange_n(&s_route_count, &idx, idx + 1, 0, __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED));

	struct ove_httpd_route *r = &s_routes[idx];
	strncpy(r->method, method, sizeof(r->method) - 1);
	r->method[sizeof(r->method) - 1] = '\0';
	strncpy(r->path, path, sizeof(r->path) - 1);
	r->path[sizeof(r->path) - 1] = '\0';
	r->handler = handler;
	__atomic_store_n(&r->ready, 1, __ATOMIC_RELEASE);

	return OVE_OK;
}

/* ---------- Path segment parsing ---------- */

static void parse_path_segments(struct ove_httpd_req *req)
{
	req->segment_count = 0;

	/* Work on a mutable copy to preserve path for route matching */
	strncpy(req->seg_buf, req->path, sizeof(req->seg_buf) - 1);
	req->seg_buf[sizeof(req->seg_buf) - 1] = '\0';

	char *p = req->seg_buf;

	/* Skip leading '/' */
	if (*p == '/')
		p++;

	while (*p && req->segment_count < OVE_HTTPD_MAX_SEGMENTS) {
		req->segments[req->segment_count++] = p;

		char *slash = strchr(p, '/');
		if (!slash)
			break;

		*slash = '\0';
		p = slash + 1;
	}
}

/* ---------- Request parsing ---------- */

static int parse_request(const char *buf, size_t len, struct ove_httpd_req *req)
{
	memset(req, 0, sizeof(*req));

	/* Parse method */
	const char *sp = memchr(buf, ' ', len);
	if (!sp)
		return 0;

	size_t mlen = (size_t)(sp - buf);
	if (mlen >= sizeof(req->method))
		mlen = sizeof(req->method) - 1;
	memcpy(req->method, buf, mlen);
	req->method[mlen] = '\0';

	/* Parse path */
	sp++;
	const char *sp2 = memchr(sp, ' ', len - (size_t)(sp - buf));
	if (!sp2)
		return 0;

	size_t plen = (size_t)(sp2 - sp);
	if (plen >= sizeof(req->path))
		plen = sizeof(req->path) - 1;
	memcpy(req->path, sp, plen);
	req->path[plen] = '\0';

	/* Split query string from path */
	char *qmark = strchr(req->path, '?');
	if (qmark) {
		*qmark = '\0';
		req->query = qmark + 1;
	}

	/* Parse path segments */
	parse_path_segments(req);

	/* Find Content-Length header */
	int content_length = 0;
	const char *cl = strstr(buf, "Content-Length:");
	if (!cl)
		cl = strstr(buf, "content-length:");
	if (cl) {
		cl += 15; /* strlen("Content-Length:") */
		while (*cl == ' ')
			cl++;
		errno = 0;
		char *endp = NULL;
		unsigned long v = strtoul(cl, &endp, 10);
		if (cl == endp || errno != 0 || v > (unsigned long)CONFIG_OVE_NET_HTTPD_MAX_BODY) {
			/* Unparseable, negative, or oversized — reject */
			return -1;
		}
		content_length = (int)v;
	}

	return content_length;
}

/* ---------- Route matching ---------- */

static struct ove_httpd_route *match_route(struct ove_httpd_req *req)
{
	struct ove_httpd_route *best = NULL;
	size_t best_len = 0;

	int count = __atomic_load_n(&s_route_count, __ATOMIC_RELAXED);
	for (int i = 0; i < count; i++) {
		struct ove_httpd_route *r = &s_routes[i];

		if (!__atomic_load_n(&r->ready, __ATOMIC_ACQUIRE))
			continue;
		if (strcmp(r->method, req->method) != 0)
			continue;

		size_t rlen = strlen(r->path);
		if (strncmp(req->path, r->path, rlen) == 0) {
			/* Require a path-segment boundary so route "/api" matches
			 * "/api", "/api/x", "/api?q" — but NOT "/api_evil".  A
			 * route that itself ends in '/' (e.g. "/" or "/static/")
			 * keeps prefix semantics and matches any sub-path. */
			char after = req->path[rlen];
			int at_boundary = after == '\0' || after == '/' || after == '?' ||
					  (rlen > 0 && r->path[rlen - 1] == '/');
			/* Pick the longest matching prefix */
			if (at_boundary && rlen > best_len) {
				best = r;
				best_len = rlen;
			}
		}
	}

	return best;
}

/* ---------- Response sending ---------- */

static int send_all(ove_socket_t sock, const void *data, size_t len)
{
	const unsigned char *p = data;
	while (len) {
		size_t sent = 0;
		int rc = ove_socket_send(sock, p, len, &sent);
		if (rc != OVE_OK)
			return rc;
		if (!sent || sent > len)
			return OVE_ERR_NOT_SUPPORTED;
		p += sent;
		len -= sent;
	}
	return OVE_OK;
}

static int send_response_n(ove_httpd_resp_t *resp, ove_socket_t sock, int status,
			   const char *content_type, size_t type_len, const void *body,
			   size_t body_len, int gzip)
{
	if (!content_type || (!body && body_len) || (type_len && memchr(content_type, 0, type_len)))
		return OVE_ERR_INVALID_PARAM;
	char header[OVE_HTTPD_HEADER_SIZE];
	int prefix = snprintf(header, sizeof(header),
			      "HTTP/1.1 %s\r\nContent-Type: ", status_str(status));
	if (prefix < 0 || (size_t)prefix >= sizeof(header))
		return OVE_ERR_NO_MEMORY;
	size_t used = (size_t)prefix;
	if (type_len >= sizeof(header) - used)
		return OVE_ERR_NO_MEMORY;
	memcpy(header + used, content_type, type_len);
	used += type_len;
	int rest = snprintf(
		header + used, sizeof(header) - used,
		"\r\n%sContent-Length: %zu\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
		gzip ? "Content-Encoding: gzip\r\n" : "", body_len);
	if (rest < 0 || (size_t)rest >= sizeof(header) - used)
		return OVE_ERR_NO_MEMORY;
	used += (size_t)rest;
	if (resp)
		resp->sent = 1;
	int rc = send_all(sock, header, used);
	return rc == OVE_OK ? send_all(sock, body, body_len) : rc;
}

static int send_response(ove_socket_t sock, int status, const char *type, const void *body,
			 size_t len)
{
	return type ? send_response_n(NULL, sock, status, type, strlen(type), body, len, 0)
		    : OVE_ERR_INVALID_PARAM;
}

int ove_httpd_resp_send_n(ove_httpd_resp_t *resp, int status, const char *type, size_t type_len,
			  const void *body, size_t len)
{
	return resp ? send_response_n(resp, resp->sock, status, type, type_len, body, len, 0)
		    : OVE_ERR_INVALID_PARAM;
}
int ove_httpd_resp_send_gz_n(ove_httpd_resp_t *resp, int status, const char *type, size_t type_len,
			     const void *body, size_t len)
{
	return resp ? send_response_n(resp, resp->sock, status, type, type_len, body, len, 1)
		    : OVE_ERR_INVALID_PARAM;
}
int ove_httpd_resp_send(ove_httpd_resp_t *resp, int status, const char *type, const void *body,
			size_t len)
{
	return type ? ove_httpd_resp_send_n(resp, status, type, strlen(type), body, len)
		    : OVE_ERR_INVALID_PARAM;
}
int ove_httpd_resp_send_gz(ove_httpd_resp_t *resp, int status, const char *type, const void *body,
			   size_t len)
{
	return type ? ove_httpd_resp_send_gz_n(resp, status, type, strlen(type), body, len)
		    : OVE_ERR_INVALID_PARAM;
}
int ove_httpd_resp_json(ove_httpd_resp_t *resp, int status, const char *json)
{
	return json ? ove_httpd_resp_send(resp, status, "application/json", json, strlen(json))
		    : OVE_ERR_INVALID_PARAM;
}
int ove_httpd_resp_html(ove_httpd_resp_t *resp, int status, const char *html, size_t len)
{
	return ove_httpd_resp_send(resp, status, "text/html", html, len);
}
int ove_httpd_resp_error_n(ove_httpd_resp_t *resp, int status, const char *message, size_t len)
{
	char escaped[OVE_HTTPD_ERROR_ESCAPED_SIZE];
	char body[256];
	if (!resp)
		return OVE_ERR_INVALID_PARAM;
	int rc = json_escape_n(message, len, escaped, sizeof(escaped));
	if (rc != OVE_OK)
		return rc;
	int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", escaped);
	if (n < 0 || (size_t)n >= sizeof(body))
		return OVE_ERR_NO_SPACE;
	return ove_httpd_resp_send(resp, status, "application/json", body, (size_t)n);
}
int ove_httpd_resp_error(ove_httpd_resp_t *resp, int status, const char *message)
{
	return ove_httpd_resp_error_n(resp, status, message, message ? strlen(message) : 0);
}

/* ---------- Request accessors ---------- */

const char *ove_httpd_req_method(ove_httpd_req_t *req)
{
	return req->method;
}

const char *ove_httpd_req_path(ove_httpd_req_t *req)
{
	return req->path;
}

const char *ove_httpd_req_query(ove_httpd_req_t *req)
{
	return req->query;
}

const char *ove_httpd_req_body(ove_httpd_req_t *req)
{
	return req->body;
}

size_t ove_httpd_req_body_len(ove_httpd_req_t *req)
{
	return req->body_len;
}

const char *ove_httpd_req_segment(ove_httpd_req_t *req, int idx)
{
	if (idx < 0 || idx >= req->segment_count)
		return NULL;
	return req->segments[idx];
}

/* ---------- Server task ---------- */

static void httpd_task(void *arg)
{
	(void)arg;

	while (!ove_thread_should_stop(ove_thread_get_self())) {
		/*
		 * Accept timeout: 50 ms when WebSocket connections are
		 * active (to poll them frequently), 1000 ms otherwise.
		 */
		uint64_t accept_timeout = OVE_MS(1000);
#ifdef CONFIG_OVE_NET_HTTPD_WS
		if (ove_httpd_ws_active_count() > 0)
			accept_timeout = OVE_MS(50);
#endif

		ove_socket_t client;
		ove_socket_storage_t client_storage;
		int ret =
			ove_socket_accept(s_server_sock, &client, &client_storage, accept_timeout);

#ifdef CONFIG_OVE_NET_HTTPD_WS
		/* Poll active WebSocket connections */
		ove_httpd_ws_poll();
#endif

		if (ret != OVE_OK)
			continue;

		/* Receive request header */
		char buf[1024];
		size_t received = 0;
		ret = ove_socket_recv(client, buf, sizeof(buf) - 1, &received, OVE_MS(5000));
		if (ret != OVE_OK || received == 0) {
			ove_socket_close(client);
			continue;
		}
		buf[received] = '\0';

		/* Parse request */
		struct ove_httpd_req req;
		int content_length = parse_request(buf, received, &req);
		if (content_length < 0) {
			send_response(client, 400, "application/json",
				      "{\"error\":\"bad request\"}", 23);
			ove_socket_close(client);
			continue;
		}

#ifdef CONFIG_OVE_NET_HTTPD_WS
		/* Check for WebSocket upgrade before route dispatch */
		if (strcmp(req.method, "GET") == 0 && ove_httpd_ws_is_upgrade(buf)) {
			if (ove_httpd_ws_handshake(buf, received, req.path, client,
						   &client_storage) == 0) {
				/* Handshake succeeded — socket transferred
				 * to WS pool, do NOT close it */
				continue;
			}
			/* Handshake failed — fall through to 404 */
		}
#endif

		/* If POST with body, receive it */
		if (strcmp(req.method, "POST") == 0 && content_length > 0) {
#ifdef CONFIG_OVE_ZERO_HEAP
			char body_static[CONFIG_OVE_NET_HTTPD_MAX_BODY + 1];
			if ((size_t)content_length > CONFIG_OVE_NET_HTTPD_MAX_BODY) {
				send_response(client, 413, "application/json",
					      "{\"error\":\"body too large\"}", 25);
				ove_socket_close(client);
				continue;
			}
			req.body = body_static;
#else
			req.body = OVE_BACKEND_MALLOC((size_t)content_length + 1);
#endif
			if (req.body) {
				/* Check if part of body was already in header recv */
				const char *body_start = strstr(buf, "\r\n\r\n");
				size_t already = 0;
				if (body_start) {
					body_start += 4;
					already = received - (size_t)(body_start - buf);
					if (already > (size_t)content_length)
						already = (size_t)content_length;
					memcpy(req.body, body_start, already);
				}

				/* Read remaining body bytes */
				while (already < (size_t)content_length) {
					size_t got = 0;
					ret = ove_socket_recv(client, req.body + already,
							      (size_t)content_length - already,
							      &got, OVE_MS(5000));
					if (ret != OVE_OK || got == 0)
						break;
					already += got;
				}
				req.body[already] = '\0';
				req.body_len = already;
			}
		}

		/* Match and dispatch */
		struct ove_httpd_resp resp;
		resp.sock = client;
		resp.sent = 0;

		struct ove_httpd_route *route = match_route(&req);
		if (route) {
			int hret = route->handler(&req, &resp);
			if (hret != OVE_OK && !resp.sent) {
				int send_rc = ove_httpd_resp_json(&resp, 500,
								  "{\"error\":\"internal error\"}");
				(void)send_rc;
			}
		}

		if (!resp.sent) {
			int send_rc = ove_httpd_resp_json(&resp, 404, "{\"error\":\"not found\"}");
			(void)send_rc;
		}

		/* Clean up */
#ifndef CONFIG_OVE_ZERO_HEAP
		if (req.body) {
			OVE_BACKEND_FREE(req.body);
		}
#endif
		ove_socket_close(client);
	}

#ifdef CONFIG_OVE_NET_HTTPD_WS
	ove_httpd_ws_close_all();
#endif
	ove_socket_close(s_server_sock);
}

/* ---------- Start / stop ---------- */

int ove_httpd_start(const ove_httpd_config_t *cfg)
{
	if (s_thread != NULL)
		return OVE_ERR_BUSY;

	s_port = (cfg && cfg->port) ? cfg->port : 80;

#ifdef CONFIG_OVE_NET_HTTPD_WS
	int ret = ove_httpd_ws_init();
	if (ret != OVE_OK)
		return ret;
#else
	int ret;
#endif

	/* Open server socket */
	ret = ove_socket_open(&s_server_sock, &s_server_storage, OVE_AF_INET, OVE_SOCK_STREAM);
	if (ret != OVE_OK)
		return ret;

	/* Bind to 0.0.0.0:port */
	ove_sockaddr_t addr;
	ove_sockaddr_ipv4(&addr, 0, 0, 0, 0, s_port);
	ret = ove_socket_bind(s_server_sock, &addr);
	if (ret != OVE_OK) {
		ove_socket_close(s_server_sock);
		return ret;
	}

	/* Listen */
	ret = ove_socket_listen(s_server_sock, 4);
	if (ret != OVE_OK) {
		ove_socket_close(s_server_sock);
		return ret;
	}

	/* Spawn server thread */
#ifdef OVE_HEAP_THREAD
	ret = ove_thread_create(&s_thread, "httpd", httpd_task, NULL, OVE_PRIO_NORMAL, 8192);
#else
	static ove_thread_storage_t httpd_th_storage;
	static uint8_t __attribute__((aligned(8))) httpd_th_stack[8192];
	ret = ove_thread_init(&s_thread, &httpd_th_storage, "httpd", httpd_task, NULL,
			      OVE_PRIO_NORMAL, sizeof(httpd_th_stack), httpd_th_stack);
#endif
	if (ret != OVE_OK) {
		s_thread = NULL; /* spawn failed — keep stop() idempotent */
		ove_socket_close(s_server_sock);
		return ret;
	}

	return OVE_OK;
}

void ove_httpd_stop(void)
{
	if (s_thread == NULL) {
		return; /* not running / already stopped — idempotent */
	}

	/* Signal the accept loop to exit, then block until the server task has
	 * actually returned (on its way out it closes s_server_sock).  Without
	 * the join, stop() returned while the task was still mid-accept or
	 * handling a request, racing a caller that frees/reinits — a teardown
	 * use-after-free.  An idle server stops within one accept timeout;
	 * an in-flight request can additionally wait for its socket timeout.
	 *
	 * Must not be called from the server task itself (would self-deadlock). */
	ove_thread_request_stop(s_thread);
#ifdef OVE_HEAP_THREAD
	ove_thread_destroy(s_thread);
#else
	ove_thread_deinit(s_thread);
#endif
	s_thread = NULL;
}
