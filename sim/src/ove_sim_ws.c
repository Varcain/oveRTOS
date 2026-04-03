/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Lightweight WebSocket + HTTP server for the oveRTOS sim dashboard.
 *
 * Architecture:
 *   - A single server thread owns all client sockets (accept, read,
 *     write, close).  No other thread touches fds.
 *   - Other threads (LVGL, audio) post frames into a mailbox
 *     (pending_frame) protected by a mutex.
 *   - The server thread drains the mailbox each poll iteration and
 *     writes WS frames to clients.
 */

#include "ove_sim_ws.h"
#include "ove/sim/ove_sim_plugin.h"
#include "ove/types.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* ── SHA-1 for WebSocket handshake ─────────────────────────────────── */

struct sha1_ctx {
	uint32_t state[5];
	uint64_t count;
	uint8_t  buffer[64];
};

static void sha1_init(struct sha1_ctx *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->count = 0;
}

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
	uint32_t w[80];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)block[i * 4] << 24) |
		       ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) |
		       ((uint32_t)block[i * 4 + 3]);
	for (int i = 16; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	uint32_t a = state[0], b = state[1], c = state[2];
	uint32_t d = state[3], e = state[4];

	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		uint32_t tmp = ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL(b, 30); b = a; a = tmp;
	}
	state[0] += a; state[1] += b; state[2] += c;
	state[3] += d; state[4] += e;
}

static void sha1_update(struct sha1_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	size_t idx = (size_t)(ctx->count & 63);
	ctx->count += len;

	for (size_t i = 0; i < len; i++) {
		ctx->buffer[idx++] = p[i];
		if (idx == 64) {
			sha1_transform(ctx->state, ctx->buffer);
			idx = 0;
		}
	}
}

static void sha1_final(struct sha1_ctx *ctx, uint8_t digest[20])
{
	uint64_t total_bits = ctx->count * 8;

	uint8_t pad[64] = {0};
	size_t idx = (size_t)(ctx->count & 63);

	pad[0] = 0x80;
	size_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
	sha1_update(ctx, pad, pad_len);

	uint8_t bits[8];
	for (int i = 7; i >= 0; i--) {
		bits[i] = (uint8_t)(total_bits & 0xFF);
		total_bits >>= 8;
	}
	sha1_update(ctx, bits, 8);

	for (int i = 0; i < 5; i++) {
		digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
		digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

/* ── Base64 encoding ───────────────────────────────────────────────── */

static const char b64_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *in, size_t in_len,
			 char *out, size_t out_size)
{
	size_t out_len = 4 * ((in_len + 2) / 3);
	if (out_len + 1 > out_size)
		return -1;

	size_t j = 0;
	for (size_t i = 0; i < in_len; i += 3) {
		uint32_t n = ((uint32_t)in[i]) << 16;
		if (i + 1 < in_len) n |= ((uint32_t)in[i + 1]) << 8;
		if (i + 2 < in_len) n |= (uint32_t)in[i + 2];

		out[j++] = b64_table[(n >> 18) & 0x3F];
		out[j++] = b64_table[(n >> 12) & 0x3F];
		out[j++] = (i + 1 < in_len) ? b64_table[(n >> 6) & 0x3F] : '=';
		out[j++] = (i + 2 < in_len) ? b64_table[n & 0x3F] : '=';
	}
	out[j] = '\0';
	return (int)j;
}

/* ── Pending-frame mailbox (any thread -> server thread) ───────────── */

/*
 * Double-buffered mailbox so the producer (LVGL/audio thread) can
 * write a new frame without blocking while the server thread sends
 * the previous one.  Only the latest frame matters for display.
 */

#define MAILBOX_BUF_SIZE (1024 * 1024) /* 1 MB — enough for 480x272 XRGB8888 */

struct mailbox {
	pthread_mutex_t lock;
	uint8_t        *buf;       /* Pending frame data (type hdr + payload) */
	size_t          len;       /* 0 = no pending frame */
};

static struct mailbox mbox = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int mailbox_init(void)
{
	mbox.buf = malloc(MAILBOX_BUF_SIZE);
	if (!mbox.buf)
		return -1;
	mbox.len = 0;
	return 0;
}

/**
 * Post a frame into the mailbox (called from any thread).
 * Overwrites the previous frame if the server hasn't consumed it yet.
 */
static void mailbox_post(enum ove_sim_ws_frame_type type,
			 const void *payload, size_t len)
{
	size_t total = 4 + len;
	if (total > MAILBOX_BUF_SIZE)
		return; /* Frame too large, drop. */

	pthread_mutex_lock(&mbox.lock);
	uint32_t t = (uint32_t)type;
	memcpy(mbox.buf, &t, 4);
	memcpy(mbox.buf + 4, payload, len);
	mbox.len = total;
	pthread_mutex_unlock(&mbox.lock);
}

/**
 * Drain the mailbox (called from server thread only).
 * Returns a pointer to the frame data and its length.
 * The returned pointer is valid until the next drain call.
 * Returns NULL if no frame pending.
 */
static uint8_t *drain_buf;

static uint8_t *mailbox_drain(size_t *out_len)
{
	pthread_mutex_lock(&mbox.lock);
	if (mbox.len == 0) {
		pthread_mutex_unlock(&mbox.lock);
		return NULL;
	}

	/* Swap into drain buffer. */
	if (!drain_buf) {
		drain_buf = malloc(MAILBOX_BUF_SIZE);
		if (!drain_buf) {
			pthread_mutex_unlock(&mbox.lock);
			return NULL;
		}
	}
	memcpy(drain_buf, mbox.buf, mbox.len);
	*out_len = mbox.len;
	mbox.len = 0;
	pthread_mutex_unlock(&mbox.lock);

	return drain_buf;
}

/* ── Client state ──────────────────────────────────────────────────── */

enum client_state {
	CLIENT_STATE_HTTP,
	CLIENT_STATE_WEBSOCKET,
};

struct ws_client {
	int              fd;
	enum client_state state;
	uint8_t          recv_buf[4096 + 1];
	size_t           recv_len;
};

/* ── Server state (all accessed only from server thread) ───────────── */

static int server_fd = -1;
static struct ws_client clients[OVE_SIM_WS_MAX_CLIENTS];
static int client_count;
static pthread_t server_thread;
static volatile int server_running;
static char dashboard_dir[512];
static volatile int ws_client_connected; /* atomic-ish flag for has_clients */

/* ── Helpers ───────────────────────────────────────────────────────── */

static void set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_blocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

static void client_close(int idx)
{
	if (clients[idx].fd >= 0) {
		close(clients[idx].fd);
		clients[idx].fd = -1;
	}
	for (int i = idx; i < client_count - 1; i++)
		clients[i] = clients[i + 1];
	client_count--;

	/* Update connected flag. */
	ws_client_connected = 0;
	for (int i = 0; i < client_count; i++) {
		if (clients[i].state == CLIENT_STATE_WEBSOCKET) {
			ws_client_connected = 1;
			break;
		}
	}
}

/* ── Blocking write helper (server thread only) ────────────────────── */

static int write_all(int fd, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	size_t remaining = len;

	while (remaining > 0) {
		ssize_t w = write(fd, p, remaining);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				usleep(200);
				continue;
			}
			return -1;
		}
		p += w;
		remaining -= (size_t)w;
	}
	return 0;
}

/* ── HTTP static file serving ──────────────────────────────────────── */

static const char *mime_type(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return "application/octet-stream";
	if (strcmp(ext, ".html") == 0) return "text/html";
	if (strcmp(ext, ".js") == 0) return "application/javascript";
	if (strcmp(ext, ".css") == 0) return "text/css";
	if (strcmp(ext, ".png") == 0) return "image/png";
	if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
	if (strcmp(ext, ".ico") == 0) return "image/x-icon";
	return "application/octet-stream";
}

/**
 * Serve a static file as a single buffered HTTP response.
 * Dashboard files are small (<64 KB), so we read the whole file into
 * memory, prepend the HTTP header, and send everything in one
 * blocking write_all() call.  This avoids partial-send issues and
 * keeps the server thread unblocked between requests.
 */
static void serve_file(int fd, const char *url_path)
{
	if (strcmp(url_path, "/") == 0)
		url_path = "/index.html";

	if (strstr(url_path, "..") != NULL) {
		const char *r = "HTTP/1.1 403 Forbidden\r\n\r\n";
		(void)write(fd, r, strlen(r));
		return;
	}

	char file_path[1024];
	snprintf(file_path, sizeof(file_path), "%s%s", dashboard_dir,
		 url_path);

	struct stat st;
	if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
		const char *r = "HTTP/1.1 404 Not Found\r\n\r\n";
		(void)write(fd, r, strlen(r));
		return;
	}

	if (st.st_size > 256 * 1024) {
		const char *r = "HTTP/1.1 413 Payload Too Large\r\n\r\n";
		(void)write(fd, r, strlen(r));
		return;
	}

	FILE *f = fopen(file_path, "rb");
	if (!f) {
		const char *r = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
		(void)write(fd, r, strlen(r));
		return;
	}

	/* Build complete response in memory. */
	char header[512];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %ld\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: close\r\n"
		"\r\n",
		mime_type(url_path), (long)st.st_size);

	size_t total = (size_t)hlen + (size_t)st.st_size;
	uint8_t *resp = malloc(total);
	if (!resp) {
		fclose(f);
		return;
	}

	memcpy(resp, header, (size_t)hlen);
	size_t body_read = fread(resp + hlen, 1, (size_t)st.st_size, f);
	fclose(f);
	(void)body_read;

	/* Single blocking send of the entire response. */
	set_blocking(fd);
	(void)write_all(fd, resp, total);
	free(resp);
}

/* ── WebSocket handshake ───────────────────────────────────────────── */

static const char ws_magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static int ws_handshake(struct ws_client *c)
{
	const char *key_hdr = strstr((char *)c->recv_buf, "Sec-WebSocket-Key: ");
	if (!key_hdr)
		return -1;

	key_hdr += 19;
	const char *key_end = strstr(key_hdr, "\r\n");
	if (!key_end)
		return -1;

	char concat[128];
	size_t key_len = (size_t)(key_end - key_hdr);
	if (key_len + sizeof(ws_magic) > sizeof(concat))
		return -1;
	memcpy(concat, key_hdr, key_len);
	memcpy(concat + key_len, ws_magic, sizeof(ws_magic));

	struct sha1_ctx sha;
	uint8_t digest[20];
	sha1_init(&sha);
	sha1_update(&sha, concat, key_len + sizeof(ws_magic) - 1);
	sha1_final(&sha, digest);

	char accept_b64[32];
	base64_encode(digest, 20, accept_b64, sizeof(accept_b64));

	char resp[256];
	int rlen = snprintf(resp, sizeof(resp),
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n"
		"\r\n",
		accept_b64);

	/* Set blocking for handshake, then back to non-blocking for poll. */
	set_blocking(c->fd);
	(void)write_all(c->fd, resp, (size_t)rlen);
	set_nonblocking(c->fd);

	c->state = CLIENT_STATE_WEBSOCKET;
	c->recv_len = 0;
	ws_client_connected = 1;
	return 0;
}

/* ── WebSocket frame I/O (server thread only) ──────────────────────── */

/**
 * Send a WS binary frame.  Uses a single writev() call to avoid
 * partial-frame issues.  Non-blocking: if the socket buffer is too
 * full the frame is dropped (acceptable for live dashboard data).
 */
static int ws_send_binary(int fd, const void *data, size_t len)
{
	uint8_t header[10];
	size_t hlen;

	header[0] = 0x82; /* FIN + binary opcode */

	if (len <= 125) {
		header[1] = (uint8_t)len;
		hlen = 2;
	} else if (len <= 65535) {
		header[1] = 126;
		header[2] = (uint8_t)(len >> 8);
		header[3] = (uint8_t)(len & 0xFF);
		hlen = 4;
	} else {
		header[1] = 127;
		for (int i = 0; i < 8; i++)
			header[2 + i] = (uint8_t)(len >> ((7 - i) * 8));
		hlen = 10;
	}

	/*
	 * Enlarge the socket send buffer to fit the entire WS frame,
	 * then do a single blocking write.  This avoids the server
	 * thread being blocked for long: the kernel accepts the full
	 * frame into the enlarged buffer and returns quickly.
	 */
	size_t total = hlen + len;
	int sndbuf = (int)(total * 2);
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	/* Use writev for atomic header+payload. */
	struct iovec iov[2] = {
		{ .iov_base = header, .iov_len = hlen },
		{ .iov_base = (void *)data, .iov_len = len },
	};

	set_blocking(fd);
	ssize_t w = writev(fd, iov, 2);
	set_nonblocking(fd);

	return ((size_t)w == total) ? 0 : -1;
}

static int ws_parse_frame(struct ws_client *c, uint8_t **out_payload,
			  size_t *out_len, uint8_t *out_opcode)
{
	if (c->recv_len < 2)
		return 0;

	uint8_t *buf = c->recv_buf;
	*out_opcode = buf[0] & 0x0F;
	int masked = (buf[1] & 0x80) != 0;
	uint64_t payload_len = buf[1] & 0x7F;
	size_t header_len = 2;

	if (payload_len == 126) {
		if (c->recv_len < 4) return 0;
		payload_len = ((uint64_t)buf[2] << 8) | buf[3];
		header_len = 4;
	} else if (payload_len == 127) {
		if (c->recv_len < 10) return 0;
		payload_len = 0;
		for (int i = 0; i < 8; i++)
			payload_len = (payload_len << 8) | buf[2 + i];
		header_len = 10;
	}

	size_t mask_len = masked ? 4 : 0;
	size_t total = header_len + mask_len + (size_t)payload_len;
	if (c->recv_len < total)
		return 0;

	uint8_t *mask_key = buf + header_len;
	uint8_t *payload = buf + header_len + mask_len;

	if (masked) {
		for (size_t i = 0; i < (size_t)payload_len; i++)
			payload[i] ^= mask_key[i & 3];
	}

	*out_payload = payload;
	*out_len = (size_t)payload_len;
	return (int)total;
}

/* ── HTTP request handling ─────────────────────────────────────────── */

static void handle_http(struct ws_client *c)
{
	c->recv_buf[c->recv_len] = '\0';

	char *end = strstr((char *)c->recv_buf, "\r\n\r\n");
	if (!end)
		return;

	char method[8] = {0};
	char path[256] = {0};
	sscanf((char *)c->recv_buf, "%7s %255s", method, path);

	if (strcmp(method, "GET") == 0 && strcmp(path, "/ws") == 0) {
		if (ws_handshake(c) != 0) {
			const char *resp = "HTTP/1.1 400 Bad Request\r\n\r\n";
			(void)write(c->fd, resp, strlen(resp));
			close(c->fd);
			c->fd = -1;
		}
		return;
	}

	serve_file(c->fd, path);
	close(c->fd);
	c->fd = -1;
}

/* ── Event pump (reads transport events, sends to WS clients) ──────── */

extern int ove_sim_direct_read_event(void *buf, size_t buf_size,
				     uint16_t *out_len, uint32_t timeout_ms);
extern int ove_sim_direct_write_cmd(const void *data, uint16_t len);

static void broadcast_to_ws(const void *frame, size_t frame_len)
{
	for (int i = 0; i < client_count; i++) {
		if (clients[i].state == CLIENT_STATE_WEBSOCKET) {
			if (ws_send_binary(clients[i].fd, frame, frame_len) < 0) {
				client_close(i);
				i--;
			}
		}
	}
}

static void pump_events(void)
{
	uint8_t buf[4096];
	uint16_t len = 0;

	while (ove_sim_direct_read_event(buf, sizeof(buf), &len, 0) == 0
	       && len > 0) {
		uint8_t frame[4100];
		uint32_t type = OVE_SIM_WS_FRAME_EVENT;
		memcpy(frame, &type, 4);
		memcpy(frame + 4, buf, len);
		broadcast_to_ws(frame, 4 + len);
		len = 0;
	}
}

static void pump_mailbox(void)
{
	size_t len;
	uint8_t *frame = mailbox_drain(&len);
	if (frame)
		broadcast_to_ws(frame, len);
}

/* ── Server thread ─────────────────────────────────────────────────── */

static void *server_loop(void *arg)
{
	(void)arg;

	while (server_running) {
		struct pollfd fds[1 + OVE_SIM_WS_MAX_CLIENTS];
		int nfds = 0;

		fds[0].fd = server_fd;
		fds[0].events = POLLIN;
		nfds = 1;

		int polled_clients = client_count;
		for (int i = 0; i < polled_clients; i++) {
			fds[nfds].fd = clients[i].fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		int ret = poll(fds, (nfds_t)nfds, 16 /* ~60 Hz */);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* Accept all pending connections. */
		if (fds[0].revents & POLLIN) {
			for (;;) {
				struct sockaddr_in addr;
				socklen_t alen = sizeof(addr);
				int cfd = accept(server_fd,
						 (struct sockaddr *)&addr,
						 &alen);
				if (cfd < 0)
					break;
				if (client_count < OVE_SIM_WS_MAX_CLIENTS) {
					set_nonblocking(cfd);
					int flag = 1;
					setsockopt(cfd, IPPROTO_TCP,
						   TCP_NODELAY, &flag,
						   sizeof(flag));
					struct ws_client *c =
						&clients[client_count++];
					c->fd = cfd;
					c->state = CLIENT_STATE_HTTP;
					c->recv_len = 0;
				} else {
					close(cfd);
				}
			}
		}

		/* Process client data — only clients that were in the
		 * poll set (not newly accepted ones). */
		for (int i = 0; i < polled_clients && i < client_count; i++) {
			int poll_idx = i + 1;
			if (!(fds[poll_idx].revents & POLLIN))
				continue;

			struct ws_client *c = &clients[i];
			size_t space = 4096 - c->recv_len;
			if (space == 0) {
				client_close(i);
				polled_clients--;
				i--;
				continue;
			}
			ssize_t n = read(c->fd,
					 c->recv_buf + c->recv_len,
					 space);
			if (n <= 0) {
				client_close(i);
				polled_clients--;
				i--;
				continue;
			}
			c->recv_len += (size_t)n;

			if (c->state == CLIENT_STATE_HTTP) {
				handle_http(c);
				if (c->fd < 0) {
					client_close(i);
					polled_clients--;
					i--;
				}
			} else {
				uint8_t *payload;
				size_t plen;
				uint8_t opcode;
				int consumed;

				while ((consumed = ws_parse_frame(c, &payload,
								  &plen,
								  &opcode))
				       > 0) {
					if (opcode == 0x08) {
						client_close(i);
						polled_clients--;
						i--;
						break;
					}
					if (opcode == 0x09) {
						uint8_t pong[2] = {0x8A, 0x00};
						(void)write(c->fd, pong, 2);
					}
					if (opcode == 0x02 && plen >= 4) {
						uint32_t ftype;
						memcpy(&ftype, payload, 4);
						if (ftype ==
						    OVE_SIM_WS_FRAME_CMD) {
							ove_sim_direct_write_cmd(
								payload + 4,
								(uint16_t)(plen - 4));
						}
					}
					size_t rem = c->recv_len -
						     (size_t)consumed;
					if (rem > 0)
						memmove(c->recv_buf,
							c->recv_buf + consumed,
							rem);
					c->recv_len = rem;
				}
			}
		}

		/* Send pending frames from mailbox + transport events. */
		pump_mailbox();
		pump_events();
	}

	return NULL;
}

/* ── Public API ────────────────────────────────────────────────────── */

int ove_sim_ws_start(uint16_t port, const char *dash_path)
{
	if (server_running)
		return OVE_ERR_INVALID_PARAM;

	snprintf(dashboard_dir, sizeof(dashboard_dir), "%s", dash_path);

	if (mailbox_init() < 0)
		return OVE_ERR_NO_MEMORY;

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0)
		return OVE_ERR_NOT_SUPPORTED;

	int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};

	if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(server_fd);
		server_fd = -1;
		return OVE_ERR_NET_ADDR_IN_USE;
	}

	if (listen(server_fd, 8) < 0) {
		close(server_fd);
		server_fd = -1;
		return OVE_ERR_NOT_SUPPORTED;
	}

	set_nonblocking(server_fd);
	server_running = 1;
	client_count = 0;

	if (pthread_create(&server_thread, NULL, server_loop, NULL) != 0) {
		close(server_fd);
		server_fd = -1;
		server_running = 0;
		return OVE_ERR_NOT_SUPPORTED;
	}

	printf("[sim] Dashboard: http://127.0.0.1:%u\n", port);
	return OVE_OK;
}

void ove_sim_ws_stop(void)
{
	if (!server_running)
		return;

	server_running = 0;
	pthread_join(server_thread, NULL);

	for (int i = 0; i < client_count; i++) {
		if (clients[i].fd >= 0)
			close(clients[i].fd);
	}
	client_count = 0;

	if (server_fd >= 0) {
		close(server_fd);
		server_fd = -1;
	}

	free(mbox.buf);
	mbox.buf = NULL;
	free(drain_buf);
	drain_buf = NULL;
}

/**
 * Post a frame to the mailbox for the server thread to send.
 * Safe to call from any thread.  Overwrites the previous unsent frame.
 */
int ove_sim_ws_broadcast(enum ove_sim_ws_frame_type type,
			 const void *payload, size_t len)
{
	if (!ws_client_connected)
		return -1;

	mailbox_post(type, payload, len);
	return OVE_OK;
}

int ove_sim_ws_has_clients(void)
{
	return ws_client_connected;
}
