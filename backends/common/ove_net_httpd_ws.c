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
 * connection pool, polled from the httpd accept loop.
 *
 * Any thread may send (every OVE_LOG line is broadcast to /ws/log), so one
 * mutex covers each frame written and every change of a slot's state; it
 * is never held across a user callback.  Slot sockets are non-blocking and
 * a frame must go out within WS_SEND_TIMEOUT_MS, so a client that stops
 * reading holds the mutex (and every other sender) that long, once.  Only
 * the httpd thread frees a slot: a failed or timed-out send just marks it
 * closing, and the next poll (or stop) calls its on_close there and then
 * closes the socket, so a connection handle stays valid until its on_close
 * returns.  On FreeRTOS this relies on lwIP's core locking, as the httpd
 * thread receives on a socket other threads send on.
 */

#include "ove/ove.h"
#include "ove/net_httpd.h"
#include "ove_backend_common.h"
#include "ove_net_httpd_ws_internal.h"

#include "ove_sha1.h"
#include "ove_base64.h"

#include <string.h>
#include <stdio.h>

#ifdef CONFIG_OVE_NET_HTTPD_WS

/* ---------- WebSocket opcodes (RFC 6455 Section 5.2) ---------- */

#define WS_OP_CONT 0x0
#define WS_OP_TEXT 0x1
#define WS_OP_BIN 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA

/* ---------- Connection pool ---------- */

#ifndef CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS
#define CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS 4
#endif

#ifndef CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME
#define CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME 1024
#endif

/* How long a frame may take to leave, and to arrive once it has started. */
#define WS_SEND_TIMEOUT_MS 1000
#define WS_RECV_TIMEOUT_MS 1000

enum ws_state {
	WS_FREE,     /* no connection */
	WS_RESERVED, /* taken by a handshake in progress (httpd thread) */
	WS_OPEN,     /* upgraded; any thread may send */
	WS_CLOSING,  /* failed or closing; the httpd thread frees it */
};

struct ove_httpd_ws_conn {
	ove_socket_t sock; /* points at storage once upgraded */
	ove_socket_storage_t storage;
	char path[64];
	uint8_t frame_buf[CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME];
	int state; /* enum ws_state, under s_ws_lock */
};

static struct ove_httpd_ws_conn s_ws_pool[CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS];

static ove_mutex_t s_ws_lock;
#ifndef OVE_HEAP_SYNC
static ove_mutex_storage_t s_ws_lock_storage;
#endif
static int s_ws_ready; /* the lock exists; set once by ove_httpd_start() */

int ove_httpd_ws_init(void)
{
	if (__atomic_load_n(&s_ws_ready, __ATOMIC_ACQUIRE))
		return OVE_OK;
#ifdef OVE_HEAP_SYNC
	int rc = ove_mutex_create(&s_ws_lock);
#else
	int rc = ove_mutex_init(&s_ws_lock, &s_ws_lock_storage);
#endif
	if (rc == OVE_OK)
		__atomic_store_n(&s_ws_ready, 1, __ATOMIC_RELEASE);
	return rc;
}

/* False before the first ove_httpd_start(), when no connection can exist. */
static int ws_lock(void)
{
	if (!__atomic_load_n(&s_ws_ready, __ATOMIC_ACQUIRE))
		return 0;
	return ove_mutex_lock(s_ws_lock, OVE_WAIT_FOREVER) == OVE_OK;
}

static void ws_unlock(void)
{
	ove_mutex_unlock(s_ws_lock);
}

/* ---------- WebSocket route table ---------- */

#define WS_MAX_ROUTES 4

/* Added at any time, as the HTTP routes are: claim a slot, fill it, then
 * set ready. */
struct ws_route {
	char path[64];
	ove_httpd_ws_handler_t on_message;
	ove_httpd_ws_close_handler_t on_close;
	int ready;
};

static struct ws_route s_ws_routes[WS_MAX_ROUTES];
static int s_ws_route_count;

/* ---------- Pool management ---------- */

/* Takes a free slot for a handshake; NULL when all are in use. */
static struct ove_httpd_ws_conn *ws_pool_reserve(void)
{
	struct ove_httpd_ws_conn *conn = NULL;

	if (!ws_lock())
		return NULL;
	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		if (s_ws_pool[i].state == WS_FREE) {
			conn = &s_ws_pool[i];
			conn->state = WS_RESERVED;
			break;
		}
	}
	ws_unlock();
	return conn;
}

static void ws_set_state(struct ove_httpd_ws_conn *conn, int state)
{
	if (ws_lock()) {
		conn->state = state;
		ws_unlock();
	}
}

int ove_httpd_ws_active_count(void)
{
	int count = 0;

	if (!ws_lock())
		return 0;
	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		if (s_ws_pool[i].state != WS_FREE)
			count++;
	}
	ws_unlock();
	return count;
}

/* ---------- Route registration ---------- */

int ove_httpd_ws_route(const char *path, ove_httpd_ws_handler_t on_message,
		       ove_httpd_ws_close_handler_t on_close)
{
	if (!path)
		return OVE_ERR_INVALID_PARAM;

	int idx = __atomic_load_n(&s_ws_route_count, __ATOMIC_RELAXED);
	do {
		if (idx >= WS_MAX_ROUTES)
			return OVE_ERR_NO_MEMORY;
	} while (!__atomic_compare_exchange_n(&s_ws_route_count, &idx, idx + 1, 0, __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED));

	struct ws_route *r = &s_ws_routes[idx];
	strncpy(r->path, path, sizeof(r->path) - 1);
	r->path[sizeof(r->path) - 1] = '\0';
	r->on_message = on_message;
	r->on_close = on_close;
	__atomic_store_n(&r->ready, 1, __ATOMIC_RELEASE);

	return OVE_OK;
}

static struct ws_route *ws_match_route(const char *path)
{
	struct ws_route *best = NULL;
	size_t best_len = 0;

	int count = __atomic_load_n(&s_ws_route_count, __ATOMIC_RELAXED);
	for (int i = 0; i < count; i++) {
		if (!__atomic_load_n(&s_ws_routes[i].ready, __ATOMIC_ACQUIRE))
			continue;
		size_t rlen = strlen(s_ws_routes[i].path);
		if (strncmp(path, s_ws_routes[i].path, rlen) == 0 && rlen > best_len) {
			best = &s_ws_routes[i];
			best_len = rlen;
		}
	}
	return best;
}

/* Waits until @p sock is ready for @p events or @p deadline_ns passes. */
static int ws_wait(ove_socket_t sock, unsigned events, uint64_t deadline_ns)
{
	uint64_t now = ove_time_now_steady_ns();
	if (now >= deadline_ns)
		return OVE_ERR_TIMEOUT;
	unsigned revents = 0;
	int ret = ove_socket_poll(sock, events, &revents, deadline_ns - now);
	if (ret != OVE_OK)
		return ret;
	if (revents & events)
		return OVE_OK;
	return (revents & (OVE_SOCK_POLLERR | OVE_SOCK_POLLHUP)) ? OVE_ERR_NET_RESET
								 : OVE_ERR_TIMEOUT;
}

static int ws_would_block(int ret)
{
	return ret == OVE_ERR_TIMEOUT || ret == OVE_ERR_WOULD_BLOCK;
}

/* Sends the full buffer on a non-blocking socket within @p deadline_ns,
 * looping over partial sends: one call may take fewer than @p len bytes. */
static int ws_send_by(ove_socket_t sock, const void *data, size_t len, uint64_t deadline_ns)
{
	const uint8_t *p = data;
	while (len > 0) {
		size_t sent = 0;
		int ret = ove_socket_send(sock, p, len, &sent);
		if (ret == OVE_OK && sent > 0) {
			p += sent;
			len -= sent;
			continue;
		}
		if (ret != OVE_OK && !ws_would_block(ret))
			return ret;
		ret = ws_wait(sock, OVE_SOCK_POLLOUT, deadline_ns);
		if (ret != OVE_OK)
			return ret;
	}
	return OVE_OK;
}

static uint64_t ws_send_deadline(void)
{
	return ove_time_now_steady_ns() + OVE_MS(WS_SEND_TIMEOUT_MS);
}

static int ws_send_all(ove_socket_t sock, const void *data, size_t len)
{
	return ws_send_by(sock, data, len, ws_send_deadline());
}

/* ---------- Handshake (RFC 6455 Section 4.2.2) ---------- */

static const char ws_magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int ove_httpd_ws_handshake(const char *headers, size_t headers_len, const char *path,
			   ove_socket_t sock, ove_socket_storage_t *storage)
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
	while (key_hdr[ki] && key_hdr[ki] != '\r' && key_hdr[ki] != '\n' && ki < 63) {
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

	/* Reserve a connection slot */
	struct ove_httpd_ws_conn *conn = ws_pool_reserve();
	if (!conn)
		return -1;
	/* Before the 101, so a client that does not read cannot hold the httpd
	 * thread; the mode moves with the socket into the slot. */
	if (ove_socket_set_nonblock(sock, 1) != OVE_OK) {
		ws_set_state(conn, WS_FREE);
		return -1;
	}

	/* Send 101 Switching Protocols */
	char resp[256];
	int rlen = snprintf(resp, sizeof(resp),
			    "HTTP/1.1 101 Switching Protocols\r\n"
			    "Upgrade: websocket\r\n"
			    "Connection: Upgrade\r\n"
			    "Sec-WebSocket-Accept: %s\r\n"
			    "\r\n",
			    accept);

	int ret = ws_send_all(sock, resp, (size_t)rlen);
	if (ret != OVE_OK) {
		ws_set_state(conn, WS_FREE);
		return -1;
	}

	/* Transfer socket ownership to the connection pool.  The accept loop's
	 * storage is its stack, reused for the next client, so the socket moves
	 * into the slot: every backend's socket is plain data (an fd, or a
	 * NuttX psock whose connection keeps no pointer back to it). */
	memcpy(&conn->storage, storage, sizeof(conn->storage));
	conn->sock = (ove_socket_t)&conn->storage;
	strncpy(conn->path, path, sizeof(conn->path) - 1);
	conn->path[sizeof(conn->path) - 1] = '\0';
	ws_set_state(conn, WS_OPEN);

	(void)headers_len;
	return 0; /* success — caller must NOT close the socket */
}

/* ---------- Frame sending (server → client, no mask) ---------- */

/* Called with the lock held; a failed write marks the slot closing. */
static int ws_send_locked(struct ove_httpd_ws_conn *conn, const void *data, size_t len)
{
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
		hdr[2] = 0;
		hdr[3] = 0;
		hdr[4] = 0;
		hdr[5] = 0;
		hdr[6] = (uint8_t)(len >> 24);
		hdr[7] = (uint8_t)(len >> 16);
		hdr[8] = (uint8_t)(len >> 8);
		hdr[9] = (uint8_t)(len);
		hlen = 10;
	}

	uint64_t deadline = ws_send_deadline();
	int ret = ws_send_by(conn->sock, hdr, (size_t)hlen, deadline);
	if (ret == OVE_OK && len > 0)
		ret = ws_send_by(conn->sock, data, len, deadline);
	if (ret != OVE_OK)
		conn->state = WS_CLOSING;
	return ret;
}

int ove_httpd_ws_send(ove_httpd_ws_conn_t *conn_opaque, const void *data, size_t len)
{
	struct ove_httpd_ws_conn *conn = (struct ove_httpd_ws_conn *)conn_opaque;

	if (!conn || (!data && len > 0))
		return OVE_ERR_INVALID_PARAM;
	if (!ws_lock())
		return OVE_ERR_NET_CLOSED;
	int ret = conn->state == WS_OPEN ? ws_send_locked(conn, data, len) : OVE_ERR_NET_CLOSED;
	ws_unlock();
	return ret;
}

/* ---------- Broadcast to all connections on a path ---------- */

int ove_httpd_ws_broadcast(const char *path, const void *data, size_t len)
{
	int sent = 0;

	if ((!data && len > 0) || !ws_lock())
		return 0;
	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		struct ove_httpd_ws_conn *c = &s_ws_pool[i];
		if (c->state != WS_OPEN)
			continue;
		if (path && strcmp(c->path, path) != 0)
			continue;
		if (ws_send_locked(c, data, len) == OVE_OK)
			sent++;
	}
	ws_unlock();
	return sent;
}

/* ---------- Frame receiving (client → server, masked) ---------- */

/* The httpd thread's own frames, under the lock like everyone else's so
 * they never land inside another thread's frame.  A closing slot gets
 * none. */
static void ws_send_control(struct ove_httpd_ws_conn *conn, const uint8_t *frame, size_t len,
			    const uint8_t *payload, size_t payload_len)
{
	if (!ws_lock())
		return;
	if (conn->state == WS_OPEN) {
		uint64_t deadline = ws_send_deadline();
		int ret = ws_send_by(conn->sock, frame, len, deadline);
		if (ret == OVE_OK && payload_len > 0)
			ret = ws_send_by(conn->sock, payload, payload_len, deadline);
		if (ret != OVE_OK)
			conn->state = WS_CLOSING;
	}
	ws_unlock();
}

static void ws_send_close(struct ove_httpd_ws_conn *conn)
{
	static const uint8_t frame[2] = {0x80 | WS_OP_CLOSE, 0};
	ws_send_control(conn, frame, sizeof(frame), NULL, 0);
}

static void ws_send_close_code(struct ove_httpd_ws_conn *conn, uint16_t code)
{
	uint8_t frame[4];
	frame[0] = 0x80 | WS_OP_CLOSE;
	frame[1] = 2;
	frame[2] = (uint8_t)(code >> 8);
	frame[3] = (uint8_t)code;
	ws_send_control(conn, frame, sizeof(frame), NULL, 0);
}

static void ws_send_pong(struct ove_httpd_ws_conn *conn, const uint8_t *payload, size_t len)
{
	uint8_t hdr[2];
	hdr[0] = 0x80 | WS_OP_PONG;
	hdr[1] = (uint8_t)len; /* ping payloads are always < 126 */
	ws_send_control(conn, hdr, sizeof(hdr), payload, len);
}

/* The rest of a frame that has started arriving, on the non-blocking slot
 * socket; -1 if it stops or the client closes. */
static int ws_recv_exact(ove_socket_t sock, void *buf, size_t len)
{
	uint64_t deadline = ove_time_now_steady_ns() + OVE_MS(WS_RECV_TIMEOUT_MS);
	size_t total = 0;
	while (total < len) {
		size_t got = 0;
		int ret = ove_socket_recv(sock, (uint8_t *)buf + total, len - total, &got,
					  OVE_MS(WS_RECV_TIMEOUT_MS));
		if (ret == OVE_OK && got == 0)
			return -1; /* closed */
		if (ret == OVE_OK) {
			total += got;
			continue;
		}
		if (!ws_would_block(ret) || ws_wait(sock, OVE_SOCK_POLLIN, deadline) != OVE_OK)
			return -1;
	}
	return 0;
}

static int ws_recv_frame(struct ove_httpd_ws_conn *conn)
{
	/* Poll is the portable non-blocking readiness primitive.  A zero
	 * receive timeout means "wait forever" on some socket backends. */
	unsigned revents = 0;
	int ret = ove_socket_poll(conn->sock, OVE_SOCK_POLLIN, &revents, 0);
	if (ret != OVE_OK)
		return -1;
	if (revents & (OVE_SOCK_POLLERR | OVE_SOCK_POLLHUP))
		return -1;
	if (!(revents & OVE_SOCK_POLLIN))
		return 0;

	/* Read first 2 bytes to get opcode + payload length. */
	uint8_t hdr[2];
	if (ws_recv_exact(conn->sock, hdr, sizeof(hdr)) != 0)
		return -1;

	int opcode = hdr[0] & 0x0F;
	int masked = (hdr[1] & 0x80) != 0;
	size_t payload_len = hdr[1] & 0x7F;

	/* RFC 6455 §5.1: server MUST fail the connection on unmasked client frames */
	if (!masked) {
		ws_send_close_code(conn, 1002);
		return -1;
	}

	int is_control = (opcode & 0x08) != 0;

	/* Extended payload length */
	if (payload_len == 126) {
		uint8_t ext[2];
		if (ws_recv_exact(conn->sock, ext, sizeof(ext)) != 0)
			return -1;
		payload_len = ((size_t)ext[0] << 8) | ext[1];
	} else if (payload_len == 127) {
		uint8_t ext[8];
		if (ws_recv_exact(conn->sock, ext, sizeof(ext)) != 0)
			return -1;
		/* Reject frames whose top 32 bits are set — we don't support >4GB */
		if (ext[0] | ext[1] | ext[2] | ext[3]) {
			ws_send_close_code(conn, 1009);
			return -1;
		}
		payload_len = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16) |
			      ((size_t)ext[6] << 8) | ext[7];
	}

	/* RFC 6455 §5.5: control frames must not exceed 125 bytes */
	if (is_control && payload_len > 125) {
		ws_send_close_code(conn, 1002);
		return -1;
	}

	/* Reject oversized data frames instead of silently truncating */
	if (payload_len > CONFIG_OVE_NET_HTTPD_WS_MAX_FRAME) {
		ws_send_close_code(conn, 1009);
		return -1;
	}

	/* Read masking key (4 bytes) — presence already enforced above */
	uint8_t mask[4] = {0};
	if (ws_recv_exact(conn->sock, mask, sizeof(mask)) != 0)
		return -1;

	/* Read payload */
	if (ws_recv_exact(conn->sock, conn->frame_buf, payload_len) != 0)
		return -1;

	/* Unmask payload */
	for (size_t i = 0; i < payload_len; i++)
		conn->frame_buf[i] ^= mask[i % 4];

	/* Handle control frames */
	if (opcode == WS_OP_CLOSE) {
		ws_send_close(conn);
		return -1; /* signal disconnect */
	}

	if (opcode == WS_OP_PING) {
		ws_send_pong(conn, conn->frame_buf, payload_len);
		return 0; /* handled internally */
	}

	if (opcode == WS_OP_PONG)
		return 0; /* ignore */

	/* Dispatch data frame to route handler */
	if (opcode == WS_OP_TEXT || opcode == WS_OP_BIN) {
		struct ws_route *route = ws_match_route(conn->path);
		if (route && route->on_message) {
			route->on_message((ove_httpd_ws_conn_t *)conn, conn->frame_buf,
					  payload_len);
		}
	}

	return 1; /* frame processed */
}

/* ---------- Poll all active connections ---------- */

static int ws_state_of(struct ove_httpd_ws_conn *conn)
{
	int state = WS_FREE;
	if (ws_lock()) {
		state = conn->state;
		ws_unlock();
	}
	return state;
}

/* httpd thread only: tell the route, then close the socket and free the
 * slot.  Senders see the slot closing from the start, so none is writing
 * to the socket when it closes. */
static void ws_reap(struct ove_httpd_ws_conn *conn)
{
	ws_set_state(conn, WS_CLOSING);
	struct ws_route *route = ws_match_route(conn->path);
	if (route && route->on_close)
		route->on_close((ove_httpd_ws_conn_t *)conn);
	if (ws_lock()) {
		ove_socket_close(conn->sock);
		conn->state = WS_FREE;
		ws_unlock();
	}
}

void ove_httpd_ws_poll(void)
{
	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		struct ove_httpd_ws_conn *c = &s_ws_pool[i];
		int state = ws_state_of(c);

		if (state == WS_CLOSING || (state == WS_OPEN && ws_recv_frame(c) < 0))
			ws_reap(c);
	}
}

void ove_httpd_ws_close_all(void)
{
	for (int i = 0; i < CONFIG_OVE_NET_HTTPD_WS_MAX_CONNS; i++) {
		struct ove_httpd_ws_conn *c = &s_ws_pool[i];
		int state = ws_state_of(c);

		if (state == WS_OPEN)
			ws_send_close_code(c, 1001); /* going away */
		if (state == WS_OPEN || state == WS_CLOSING)
			ws_reap(c);
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

	return (strstr(upgrade, "websocket") != NULL || strstr(upgrade, "WebSocket") != NULL);
}

#endif /* CONFIG_OVE_NET_HTTPD_WS */
