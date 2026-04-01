/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WebSocket (RFC 6455) support for the oveRTOS embedded HTTP server.
 *
 * Provides upgrade handshake, frame parsing/sending, and a static
 * connection pool.  Designed for single-threaded polling from the
 * httpd accept loop.
 */

#include "ove/ove.h"
#include "ove/net_httpd.h"
#include "ove_backend_common.h"

#include "ove_sha1.h"
#include "ove_base64.h"

#include <string.h>
#include <stdio.h>

#ifdef CONFIG_OVE_NET_HTTPD_WS

/* ---------- WebSocket opcodes (RFC 6455 Section 5.2) ---------- */

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* ---------- Connection pool ---------- */

#ifndef CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS
#define CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS 4
#endif

#ifndef CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME
#define CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME 1024
#endif

struct ove_httpd_ws_conn {
	ove_socket_t         sock;
	ove_socket_storage_t storage;
	char                 path[64];
	uint8_t              frame_buf[CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME];
	int                  active;
};

static struct ove_httpd_ws_conn s_ws_pool[CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS];

/* ---------- WebSocket route table ---------- */

#define WS_MAX_ROUTES 4

struct ws_route {
	char                        path[64];
	ove_httpd_ws_handler_t      on_message;
	ove_httpd_ws_close_handler_t on_close;
};

static struct ws_route s_ws_routes[WS_MAX_ROUTES];
static int s_ws_route_count;

/* ---------- Pool management ---------- */

static struct ove_httpd_ws_conn *ws_pool_alloc(void)
{
	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		if (!s_ws_pool[i].active)
			return &s_ws_pool[i];
	}
	return NULL;
}

static void ws_pool_free(struct ove_httpd_ws_conn *conn)
{
	if (conn) {
		ove_socket_close(conn->sock);
		conn->active = 0;
	}
}

int ove_httpd_ws_active_count(void)
{
	int count = 0;
	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		if (s_ws_pool[i].active)
			count++;
	}
	return count;
}

/* ---------- Route registration ---------- */

int ove_httpd_ws_route(const char *path,
		       ove_httpd_ws_handler_t on_message,
		       ove_httpd_ws_close_handler_t on_close)
{
	if (s_ws_route_count >= WS_MAX_ROUTES)
		return OVE_ERR_NO_MEMORY;

	struct ws_route *r = &s_ws_routes[s_ws_route_count];
	strncpy(r->path, path, sizeof(r->path) - 1);
	r->path[sizeof(r->path) - 1] = '\0';
	r->on_message = on_message;
	r->on_close = on_close;
	s_ws_route_count++;

	return OVE_OK;
}

static struct ws_route *ws_match_route(const char *path)
{
	struct ws_route *best = NULL;
	size_t best_len = 0;

	for (int i = 0; i < s_ws_route_count; i++) {
		size_t rlen = strlen(s_ws_routes[i].path);
		if (strncmp(path, s_ws_routes[i].path, rlen) == 0 &&
		    rlen > best_len) {
			best = &s_ws_routes[i];
			best_len = rlen;
		}
	}
	return best;
}

/* ---------- Handshake (RFC 6455 Section 4.2.2) ---------- */

static const char ws_magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int ove_httpd_ws_handshake(const char *headers, size_t headers_len,
			   const char *path,
			   ove_socket_t sock,
			   ove_socket_storage_t *storage)
{
	/* Check for a matching WS route */
	struct ws_route *route = ws_match_route(path);
	if (!route)
		return -1;

	/* Extract Sec-WebSocket-Key header */
	const char *key_hdr = strstr(headers, "Sec-WebSocket-Key:");
	if (!key_hdr)
		key_hdr = strstr(headers, "sec-websocket-key:");
	if (!key_hdr)
		return -1;

	key_hdr += 18; /* strlen("Sec-WebSocket-Key:") */
	while (*key_hdr == ' ')
		key_hdr++;

	/* Copy key value (up to CRLF) */
	char key[64];
	int ki = 0;
	while (key_hdr[ki] && key_hdr[ki] != '\r' &&
	       key_hdr[ki] != '\n' && ki < 63) {
		key[ki] = key_hdr[ki];
		ki++;
	}
	key[ki] = '\0';

	/* Trim trailing spaces */
	while (ki > 0 && key[ki - 1] == ' ')
		key[--ki] = '\0';

	/* Concatenate key + magic GUID */
	char concat[128];
	int clen = snprintf(concat, sizeof(concat), "%s%s", key, ws_magic);
	if (clen < 0 || (size_t)clen >= sizeof(concat))
		return -1;

	/* SHA-1 hash → Base64 */
	uint8_t hash[20];
	ove_sha1(concat, (size_t)clen, hash);

	char accept[32];
	ove_base64_encode(hash, 20, accept, sizeof(accept));

	/* Allocate connection slot */
	struct ove_httpd_ws_conn *conn = ws_pool_alloc();
	if (!conn)
		return -1;

	/* Send 101 Switching Protocols */
	char resp[256];
	int rlen = snprintf(resp, sizeof(resp),
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n"
		"\r\n", accept);

	int ret = ove_socket_send(sock, resp, (size_t)rlen, NULL);
	if (ret != OVE_OK) {
		conn->active = 0;
		return -1;
	}

	/* Transfer socket ownership to the connection pool */
	conn->sock = sock;
	memcpy(&conn->storage, storage, sizeof(conn->storage));
	strncpy(conn->path, path, sizeof(conn->path) - 1);
	conn->path[sizeof(conn->path) - 1] = '\0';
	conn->active = 1;

	(void)headers_len;
	return 0; /* success — caller must NOT close the socket */
}

/* ---------- Frame sending (server → client, no mask) ---------- */

int ove_httpd_ws_send(ove_httpd_ws_conn_t *conn_opaque,
		      const void *data, size_t len)
{
	struct ove_httpd_ws_conn *conn =
		(struct ove_httpd_ws_conn *)conn_opaque;

	if (!conn || !conn->active)
		return OVE_ERR_INVALID_PARAM;

	/*
	 * Build frame header:
	 *  byte 0: FIN=1, opcode=TEXT(0x1)
	 *  byte 1: MASK=0, length
	 */
	uint8_t hdr[10];
	int hlen = 0;

	hdr[0] = 0x80 | WS_OP_TEXT; /* FIN + TEXT */

	if (len < 126) {
		hdr[1] = (uint8_t)len;
		hlen = 2;
	} else if (len <= 0xFFFF) {
		hdr[1] = 126;
		hdr[2] = (uint8_t)(len >> 8);
		hdr[3] = (uint8_t)(len);
		hlen = 4;
	} else {
		/* 64-bit length — unlikely for embedded, but handle it */
		hdr[1] = 127;
		hdr[2] = 0; hdr[3] = 0; hdr[4] = 0; hdr[5] = 0;
		hdr[6] = (uint8_t)(len >> 24);
		hdr[7] = (uint8_t)(len >> 16);
		hdr[8] = (uint8_t)(len >> 8);
		hdr[9] = (uint8_t)(len);
		hlen = 10;
	}

	int ret = ove_socket_send(conn->sock, hdr, (size_t)hlen, NULL);
	if (ret != OVE_OK) {
		ws_pool_free(conn);
		return ret;
	}

	if (len > 0) {
		ret = ove_socket_send(conn->sock, data, len, NULL);
		if (ret != OVE_OK) {
			ws_pool_free(conn);
			return ret;
		}
	}

	return OVE_OK;
}

/* ---------- Broadcast to all connections on a path ---------- */

int ove_httpd_ws_broadcast(const char *path,
			   const void *data, size_t len)
{
	int sent = 0;

	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		struct ove_httpd_ws_conn *c = &s_ws_pool[i];
		if (!c->active)
			continue;
		if (path && strcmp(c->path, path) != 0)
			continue;
		if (ove_httpd_ws_send((ove_httpd_ws_conn_t *)c,
				      data, len) == OVE_OK)
			sent++;
	}

	return sent;
}

/* ---------- Frame receiving (client → server, masked) ---------- */

static void ws_send_close(struct ove_httpd_ws_conn *conn)
{
	uint8_t frame[2] = { 0x80 | WS_OP_CLOSE, 0 };
	ove_socket_send(conn->sock, frame, 2, NULL);
}

static void ws_send_pong(struct ove_httpd_ws_conn *conn,
			 const uint8_t *payload, size_t len)
{
	uint8_t hdr[2];
	hdr[0] = 0x80 | WS_OP_PONG;
	hdr[1] = (uint8_t)len; /* ping payloads are always < 126 */
	ove_socket_send(conn->sock, hdr, 2, NULL);
	if (len > 0)
		ove_socket_send(conn->sock, payload, len, NULL);
}

static int ws_recv_frame(struct ove_httpd_ws_conn *conn)
{
	/*
	 * Read first 2 bytes to get opcode + payload length.
	 * Use timeout=0 for non-blocking poll.
	 */
	uint8_t hdr[2];
	size_t got = 0;
	int ret = ove_socket_recv(conn->sock, hdr, 2, &got, 0);

	if (ret != OVE_OK || got == 0)
		return 0; /* no data available */

	if (got < 2) {
		/* Incomplete header — connection broken */
		return -1;
	}

	int opcode = hdr[0] & 0x0F;
	int masked = (hdr[1] & 0x80) != 0;
	size_t payload_len = hdr[1] & 0x7F;

	/* Extended payload length */
	if (payload_len == 126) {
		uint8_t ext[2];
		ret = ove_socket_recv(conn->sock, ext, 2, &got, 1000);
		if (ret != OVE_OK || got < 2)
			return -1;
		payload_len = ((size_t)ext[0] << 8) | ext[1];
	} else if (payload_len == 127) {
		uint8_t ext[8];
		ret = ove_socket_recv(conn->sock, ext, 8, &got, 1000);
		if (ret != OVE_OK || got < 8)
			return -1;
		payload_len = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16) |
			      ((size_t)ext[6] << 8)  | ext[7];
	}

	/* Read masking key (4 bytes) if present */
	uint8_t mask[4] = {0};
	if (masked) {
		ret = ove_socket_recv(conn->sock, mask, 4, &got, 1000);
		if (ret != OVE_OK || got < 4)
			return -1;
	}

	/* Clamp payload to buffer size */
	size_t to_read = payload_len;
	if (to_read > CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME)
		to_read = CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME;

	/* Read payload */
	size_t total = 0;
	while (total < to_read) {
		got = 0;
		ret = ove_socket_recv(conn->sock, conn->frame_buf + total,
				      to_read - total, &got, 1000);
		if (ret != OVE_OK || got == 0)
			return -1;
		total += got;
	}

	/* Discard excess bytes beyond buffer capacity */
	if (payload_len > to_read) {
		uint8_t discard[64];
		size_t excess = payload_len - to_read;
		while (excess > 0) {
			size_t chunk = excess > sizeof(discard) ? sizeof(discard) : excess;
			ret = ove_socket_recv(conn->sock, discard, chunk, &got, 1000);
			if (ret != OVE_OK || got == 0)
				return -1;
			excess -= got;
		}
	}

	/* Unmask payload */
	if (masked) {
		for (size_t i = 0; i < to_read; i++)
			conn->frame_buf[i] ^= mask[i % 4];
	}

	/* Handle control frames */
	if (opcode == WS_OP_CLOSE) {
		ws_send_close(conn);
		return -1; /* signal disconnect */
	}

	if (opcode == WS_OP_PING) {
		ws_send_pong(conn, conn->frame_buf, to_read);
		return 0; /* handled internally */
	}

	if (opcode == WS_OP_PONG)
		return 0; /* ignore */

	/* Dispatch data frame to route handler */
	if (opcode == WS_OP_TEXT || opcode == WS_OP_BIN) {
		struct ws_route *route = ws_match_route(conn->path);
		if (route && route->on_message) {
			route->on_message((ove_httpd_ws_conn_t *)conn,
					  conn->frame_buf, to_read);
		}
	}

	return 1; /* frame processed */
}

/* ---------- Poll all active connections ---------- */

void ove_httpd_ws_poll(void)
{
	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		struct ove_httpd_ws_conn *c = &s_ws_pool[i];
		if (!c->active)
			continue;

		int ret = ws_recv_frame(c);
		if (ret < 0) {
			/* Connection closed or error */
			struct ws_route *route = ws_match_route(c->path);
			if (route && route->on_close)
				route->on_close((ove_httpd_ws_conn_t *)c);
			ws_pool_free(c);
		}
	}
}

/* ---------- Check if request is a WebSocket upgrade ---------- */

int ove_httpd_ws_is_upgrade(const char *headers)
{
	const char *upgrade = strstr(headers, "Upgrade:");
	if (!upgrade)
		upgrade = strstr(headers, "upgrade:");
	if (!upgrade)
		return 0;

	return (strstr(upgrade, "websocket") != NULL ||
		strstr(upgrade, "WebSocket") != NULL);
}

#endif /* CONFIG_OVE_NET_HTTPD_WS */
