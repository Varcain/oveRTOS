/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WebSocket connection lifetime in the httpd: the real server
 * (backends/common/ove_net_httpd*.c) on the POSIX socket backend, driven by
 * raw-socket clients on 127.0.0.1.  The server keeps static state and has
 * no way to forget routes, so each case runs in a child process of its own
 * on a free port, under an alarm so that a hang fails instead of stalling
 * the lane.  Cases print what they see to stderr; the stub-tsan lane also
 * catches the races the cross-thread cases provoke.  The children _exit(),
 * so LeakSanitizer does not check the server.
 */

#include "ove/ove.h"
#include "ove/net.h"
#include "ove/net_httpd.h"
#include "ove/shell.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_port;
static unsigned long g_httpd_tid;
static int g_close_count;
static int g_close_on_httpd;
static ove_httpd_ws_conn_t *g_last_conn;
static int g_stop;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

/* The console writes to stdout; cases whose own output is noise send it
 * away (the PROVE-style notes go to stderr). */
static void quiet_stdout(void)
{
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, STDOUT_FILENO);
		close(devnull);
	}
}

/* ---------- Server-side hooks ---------- */

static void echo_on_message(ove_httpd_ws_conn_t *conn, const void *data, size_t len)
{
	__atomic_store_n(&g_httpd_tid, (unsigned long)pthread_self(), __ATOMIC_RELEASE);
	__atomic_store_n(&g_last_conn, conn, __ATOMIC_RELEASE);
	int rc = ove_httpd_ws_send(conn, data, len);
	(void)rc;
}

static void echo_on_close(ove_httpd_ws_conn_t *conn)
{
	(void)conn;
	__atomic_add_fetch(&g_close_count, 1, __ATOMIC_ACQ_REL);
	if ((unsigned long)pthread_self() == __atomic_load_n(&g_httpd_tid, __ATOMIC_ACQUIRE))
		__atomic_add_fetch(&g_close_on_httpd, 1, __ATOMIC_ACQ_REL);
}

static int start_server(void)
{
	ove_httpd_config_t cfg = {.port = (uint16_t)g_port, .max_body_size = 1024};
	int rc = ove_httpd_start(&cfg);
	if (rc != OVE_OK)
		say("PROVE setup ove_httpd_start=%d", rc);
	return rc;
}

/* ---------- Observation helpers ---------- */

/* Server-side sockets of our port in CLOSE_WAIT: the peer has gone but the
 * server still holds the fd. */
static int count_close_wait(void)
{
	FILE *f = fopen("/proc/net/tcp", "r");
	char line[256];
	int n = 0;
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		(void)fclose(f);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		unsigned sl, la, lp, ra, rp, st;
		/* NOLINTNEXTLINE(cert-err34-c): the field count is checked */
		if (sscanf(line, " %u: %x:%x %x:%x %x", &sl, &la, &lp, &ra, &rp, &st) == 6 &&
		    lp == (unsigned)g_port && st == 0x08)
			n++;
	}
	(void)fclose(f);
	return n;
}

static void hex(const uint8_t *b, size_t n, char *out, size_t cap)
{
	size_t o = 0;
	out[0] = '\0';
	for (size_t i = 0; i < n && o + 4 < cap; i++)
		o += (size_t)snprintf(out + o, cap - o, "%02X ", b[i]);
}

/* ---------- Raw-socket client ---------- */

static int cli_connect(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)g_port)};
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void cli_abort(int fd)
{
	struct linger lg = {.l_onoff = 1, .l_linger = 0};
	setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
	close(fd);
}

/* Read whatever arrives within @p ms, up to @p cap bytes; stops at EOF.
 * Returns the byte count, and *eof = 1 if the peer closed. */
static size_t cli_read(int fd, uint8_t *buf, size_t cap, unsigned ms, int *eof)
{
	size_t got = 0;
	uint64_t end = now_ms() + ms;
	*eof = 0;
	while (got < cap) {
		uint64_t t = now_ms();
		if (t >= end)
			break;
		struct pollfd p = {.fd = fd, .events = POLLIN};
		if (poll(&p, 1, (int)(end - t)) <= 0)
			break;
		ssize_t n = recv(fd, buf + got, cap - got, 0);
		if (n <= 0) {
			*eof = 1;
			break;
		}
		got += (size_t)n;
	}
	return got;
}

/* RFC 6455 section 1.3 sample key and its accept value. */
#define WS_KEY "dGhlIHNhbXBsZSBub25jZQ=="
#define WS_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

static int ws_open(const char *path)
{
	int fd = cli_connect();
	char req[512];
	char resp[512];
	if (fd < 0)
		return -1;
	int n = snprintf(req, sizeof(req),
			 "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
			 "Connection: Upgrade\r\nSec-WebSocket-Key: " WS_KEY "\r\n"
			 "Sec-WebSocket-Version: 13\r\n\r\n",
			 path);
	if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) {
		close(fd);
		return -1;
	}
	size_t got = 0;
	uint64_t end = now_ms() + 2000;
	while (got < sizeof(resp) - 1 && now_ms() < end) {
		struct pollfd p = {.fd = fd, .events = POLLIN};
		if (poll(&p, 1, 100) <= 0)
			continue;
		ssize_t r = recv(fd, resp + got, 1, 0); /* byte-wise: stop at the blank line */
		if (r <= 0)
			break;
		got++;
		resp[got] = '\0';
		if (got >= 4 && memcmp(resp + got - 4, "\r\n\r\n", 4) == 0)
			break;
	}
	resp[got] = '\0';
	if (strncmp(resp, "HTTP/1.1 101", 12) != 0 || !strstr(resp, WS_ACCEPT)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int ws_send_text(int fd, const char *s)
{
	uint8_t f[256];
	size_t len = strlen(s);
	const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
	if (len > 125)
		return -1;
	f[0] = 0x81;
	f[1] = (uint8_t)(0x80 | len);
	memcpy(f + 2, mask, 4);
	for (size_t i = 0; i < len; i++)
		f[6 + i] = (uint8_t)s[i] ^ mask[i % 4];
	return send(fd, f, 6 + len, MSG_NOSIGNAL) == (ssize_t)(6 + len) ? 0 : -1;
}

/* Wait up to @p ms for one server frame.  Returns its opcode (payload in
 * @p buf, NUL-terminated), -1 on timeout, -2 on EOF/error. */
static int ws_recv(int fd, char *buf, size_t cap, unsigned ms)
{
	uint8_t h[2];
	int eof;
	if (cli_read(fd, h, 2, ms, &eof) != 2)
		return eof ? -2 : -1;
	size_t len = h[1] & 0x7F;
	if (len == 126) {
		uint8_t e[2];
		if (cli_read(fd, e, 2, 1000, &eof) != 2)
			return -2;
		len = ((size_t)e[0] << 8) | e[1];
	}
	uint8_t tmp[2048];
	if (len > sizeof(tmp))
		return -2;
	if (len && cli_read(fd, tmp, len, 1000, &eof) != len)
		return -2;
	size_t c = len < cap - 1 ? len : cap - 1;
	memcpy(buf, tmp, c);
	buf[c] = '\0';
	return h[0] & 0x0F;
}

static const char *ws_result(int r)
{
	if (r == -1)
		return "nothing (timeout)";
	if (r == -2)
		return "EOF";
	if (r == 0x8)
		return "close frame";
	return "frame";
}

static int close_count(void)
{
	return __atomic_load_n(&g_close_count, __ATOMIC_ACQUIRE);
}

/* ---------- An HTTP request beside an open WebSocket ---------- */

/* The accepted socket lived in the accept loop's stack and the WebSocket
 * slot kept a pointer to it, so the next HTTP client's socket replaced the
 * WebSocket's: it went deaf (or answered the HTTP client with a close
 * frame) and its own socket leaked.  @p queued: the HTTP request is already
 * waiting when the server gets to it, behind a slow client. */
static int case_http_beside_ws(int queued)
{
	const char *tag = queued ? "queued" : "plain";
	char buf[256];
	int broken = 0;

	if (ove_httpd_ws_route("/ws/echo", echo_on_message, echo_on_close) != OVE_OK ||
	    start_server() != OVE_OK)
		return 2;

	int ws = ws_open("/ws/echo");
	if (ws < 0)
		return 2;
	ws_send_text(ws, "hello");
	int r = ws_recv(ws, buf, sizeof(buf), 2000);
	if (r != 1 || strcmp(buf, "hello") != 0)
		return 2;

	int slow = -1;
	if (queued) {
		slow = cli_connect();
		sleep_ms(200);
	}
	int g = cli_connect();
	if (!queued)
		sleep_ms(200); /* the server accepts and polls before the request */
	const char get[] = "GET /x HTTP/1.1\r\n\r\n";
	send(g, get, sizeof(get) - 1, MSG_NOSIGNAL);
	if (queued) {
		sleep_ms(100);
		const char gety[] = "GET /y HTTP/1.1\r\n\r\n";
		send(slow, gety, sizeof(gety) - 1, MSG_NOSIGNAL);
		uint8_t sb[256];
		int seof;
		(void)cli_read(slow, sb, sizeof(sb) - 1, 2000, &seof);
	}

	uint8_t gb[256];
	int geof;
	size_t gn = cli_read(g, gb, sizeof(gb) - 1, 2000, &geof);
	char hx[64];
	hex(gb, gn < 8 ? gn : 8, hx, sizeof(hx));
	gb[gn] = '\0';
	int got_404 = gn >= 12 && strncmp((char *)gb, "HTTP/1.1 404", 12) == 0;
	say("%s: HTTP client got %zu bytes (%s)", tag, gn, got_404 ? "404" : hx);
	broken |= !got_404;

	ws_send_text(ws, "after");
	r = ws_recv(ws, buf, sizeof(buf), 2000);
	say("%s: WebSocket echo after the HTTP request: %s '%s'", tag, ws_result(r),
	    r == 1 ? buf : "");
	broken |= r != 1 || strcmp(buf, "after") != 0;

	close(g);
	if (slow >= 0)
		close(slow);
	close(ws);
	sleep_ms(500);
	int cw = count_close_wait();
	say("%s: after the clients closed: CLOSE_WAIT=%d active=%d on_close=%d", tag, cw,
	    ove_httpd_ws_active_count(), close_count());
	broken |= cw != 0 || ove_httpd_ws_active_count() != 0 || close_count() != 1;

	ove_httpd_stop();
	return broken;
}

/* ---------- A failed send ---------- */

/* A failed send freed the slot without on_close, which then never ran. */
static int case_failed_send(int use_broadcast)
{
	const char *tag = use_broadcast ? "broadcast" : "send";
	char buf[64];

	if (ove_httpd_ws_route("/ws/echo", echo_on_message, echo_on_close) != OVE_OK ||
	    start_server() != OVE_OK)
		return 2;
	int ws = ws_open("/ws/echo");
	if (ws < 0)
		return 2;
	ws_send_text(ws, "hi");
	if (ws_recv(ws, buf, sizeof(buf), 2000) != 1)
		return 2;
	ove_httpd_ws_conn_t *conn = __atomic_load_n(&g_last_conn, __ATOMIC_ACQUIRE);

	cli_abort(ws); /* RST */
	int i;
	int rc = OVE_OK;
	for (i = 1; i <= 1000; i++) {
		if (use_broadcast) {
			rc = ove_httpd_ws_broadcast("/ws/echo", "x", 1);
			if (rc == 0)
				break;
		} else {
			rc = ove_httpd_ws_send(conn, "x", 1);
			if (rc != OVE_OK)
				break;
		}
		sleep_ms(1);
	}
	sleep_ms(500); /* several 50 ms polls */
	int oc = close_count();
	int on_httpd = __atomic_load_n(&g_close_on_httpd, __ATOMIC_ACQUIRE);
	say("%s: failed after %d call(s) rc=%d; +500 ms: active=%d on_close=%d (on httpd: %d)", tag,
	    i, rc, ove_httpd_ws_active_count(), oc, on_httpd);
	int broken = i > 1000 || ove_httpd_ws_active_count() != 0 || oc != 1 || on_httpd != 1;
	ove_httpd_stop();
	return broken;
}

/* ---------- Stop ---------- */

/* Stop left connections open without on_close, still pointing into the
 * stopped thread's stack; a restarted server then read freed memory. */
static int case_stop(int restart)
{
	const char *tag = restart ? "restart" : "stop";
	char buf[64];

	if (ove_httpd_ws_route("/ws/echo", echo_on_message, echo_on_close) != OVE_OK ||
	    start_server() != OVE_OK)
		return 2;
	int ws = ws_open("/ws/echo");
	if (ws < 0)
		return 2;
	ws_send_text(ws, "hi");
	if (ws_recv(ws, buf, sizeof(buf), 2000) != 1)
		return 2;

	ove_httpd_stop();
	int oc = close_count();
	int act = ove_httpd_ws_active_count();
	int r = ws_recv(ws, buf, sizeof(buf), 2000);
	int code = r == 0x8 ? ((uint8_t)buf[0] << 8) | (uint8_t)buf[1] : -1;
	int eof;
	uint8_t tail[8];
	(void)cli_read(ws, tail, sizeof(tail), 2000, &eof);
	say("%s: after stop: on_close=%d active=%d; client got %s (code %d)%s", tag, oc, act,
	    ws_result(r), code, eof ? " then EOF" : "");
	int broken = oc != 1 || act != 0 || code != 1001 || !eof;
	close(ws);

	if (restart) {
		if (start_server() != OVE_OK)
			return 2;
		int ws2 = ws_open("/ws/echo");
		ws_send_text(ws2, "again");
		r = ws_recv(ws2, buf, sizeof(buf), 2000);
		say("%s: restarted server echo: %s '%s'", tag, ws_result(r), r == 1 ? buf : "");
		broken |= r != 1 || strcmp(buf, "again") != 0;
		close(ws2);
		ove_httpd_stop();
		broken |= close_count() != 2;
	}
	return broken;
}

/* ---------- Other threads sending while clients come and go ---------- */

static void *broadcaster(void *arg)
{
	const char *path = arg;
	while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE))
		(void)ove_httpd_ws_broadcast(path, "bcast", 5);
	return NULL;
}

static void *logger(void *arg)
{
	(void)arg;
	for (unsigned long n = 0; !__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE); n++) {
		OVE_LOG_INF("tick %lu", n);
		sleep_ms(1);
	}
	return NULL;
}

/* A thread broadcasting (or logging, which broadcasts to /ws/log) while
 * clients open and drop connections: the pool had no lock. */
static int case_send_while_churning(int via_log)
{
	const char *tag = via_log ? "log" : "broadcast";
	const char *path = via_log ? "/ws/log" : "/ws/echo";
	pthread_t t;

	if (via_log) {
		quiet_stdout(); /* only the lines' broadcast to /ws/log matters */
		ove_httpd_register_builtin_routes();
	} else if (ove_httpd_ws_route("/ws/echo", echo_on_message, echo_on_close) != OVE_OK)
		return 2;
	if (start_server() != OVE_OK)
		return 2;
	pthread_create(&t, NULL, via_log ? logger : broadcaster, (void *)path);

	int opens = 0;
	uint64_t end = now_ms() + 2000;
	for (int i = 0; now_ms() < end; i++) {
		int ws = ws_open(path);
		if (ws < 0) {
			sleep_ms(5);
			continue;
		}
		opens++;
		char buf[64];
		(void)ws_recv(ws, buf, sizeof(buf), 20);
		if (i & 1)
			cli_abort(ws);
		else
			close(ws);
		sleep_ms(2);
	}
	__atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
	ove_httpd_stop();
	int oc = close_count();
	say("%s: %d connections, on_close=%d, active after stop=%d", tag, opens, oc,
	    ove_httpd_ws_active_count());
	/* Every connection gets its on_close (the log route has none). */
	return opens == 0 || ove_httpd_ws_active_count() != 0 || (!via_log && oc < opens);
}

/* ---------- A client that stops reading ---------- */

static void *big_broadcaster(void *arg)
{
	static char big[1000];
	(void)arg;
	memset(big, 'x', sizeof(big));
	while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
		(void)ove_httpd_ws_broadcast("/ws/echo", big, sizeof(big));
		sleep_ms(1);
	}
	return NULL;
}

/* A client whose window fills held the pool's lock in a send with no
 * timeout: every sender, the httpd thread and ove_httpd_stop() stopped
 * for good.  Now its frame times out, it is closed, and the rest go on. */
static int case_stalled_client(int unused)
{
	(void)unused;
	if (ove_httpd_ws_route("/ws/echo", echo_on_message, echo_on_close) != OVE_OK ||
	    start_server() != OVE_OK)
		return 2;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int rcvbuf = 1024;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)g_port)};
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
		return 2;
	char req[256];
	int n = snprintf(req, sizeof(req),
			 "GET /ws/echo HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
			 "Sec-WebSocket-Key: " WS_KEY "\r\n\r\n");
	(void)send(fd, req, (size_t)n, MSG_NOSIGNAL);
	sleep_ms(300); /* upgraded; from here on it never reads */

	pthread_t t;
	pthread_create(&t, NULL, big_broadcaster, NULL);
	sleep_ms(2500); /* the window fills, the frame times out, the slot closes */

	int g = cli_connect();
	const char get[] = "GET /x HTTP/1.1\r\n\r\n";
	(void)send(g, get, sizeof(get) - 1, MSG_NOSIGNAL);
	uint8_t gb[256];
	int eof;
	size_t gn = cli_read(g, gb, sizeof(gb) - 1, 3000, &eof);
	int served = gn >= 12 && strncmp((char *)gb, "HTTP/1.1 404", 12) == 0;
	close(g);

	__atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
	uint64_t t0 = now_ms();
	ove_httpd_stop();
	uint64_t stop_ms = now_ms() - t0;
	say("stalled: HTTP request served: %s; on_close=%d; stop took %llu ms",
	    served ? "yes" : "no", close_count(), (unsigned long long)stop_ms);
	close(fd);
	return !served || close_count() != 1 || stop_ms > 3000;
}

/* ---------- The web terminal ---------- */

/* Frames that arrive within @p ms (then 300 ms after each); the first one's
 * first line in @p first. */
static int shell_frames(int ws, unsigned ms, char *first, size_t cap)
{
	char buf[256];
	int n = 0;
	uint64_t end = now_ms() + 2 * ms + 200; /* a busy console can stream on */
	first[0] = '\0';
	while (now_ms() < end) {
		int r = ws_recv(ws, buf, sizeof(buf), ms);
		if (r != 1)
			break;
		if (n++ == 0) {
			(void)snprintf(first, cap, "%s", buf);
			first[strcspn(first, "\n")] = '\0';
		}
		ms = 300;
	}
	return n;
}

static void *console_help(void *arg)
{
	(void)arg;
	ove_shell_process_line("help");
	return NULL;
}

/* The terminal set a global output hook, so every later command's output,
 * the console's too, went to the last browser that had typed one. */
static int case_shell_output_scoped(int unused)
{
	char first[128];
	(void)unused;

	quiet_stdout();
	ove_httpd_register_builtin_routes();
	if (ove_shell_init() != OVE_OK || start_server() != OVE_OK)
		return 2;
	int ws = ws_open("/ws/shell");
	if (ws < 0)
		return 2;
	ws_send_text(ws, "help");
	int own = shell_frames(ws, 2000, first, sizeof(first));
	say("terminal: its own 'help' gave %d frame(s), first '%s'", own, first);

	pthread_t t;
	pthread_create(&t, NULL, console_help, NULL);
	pthread_join(t, NULL);
	int leaked = shell_frames(ws, 1000, first, sizeof(first));
	say("terminal: the console's 'help' gave it %d frame(s)", leaked);
	close(ws);
	ove_httpd_stop();
	return own == 0 || leaked != 0;
}

static void *console_loop(void *arg)
{
	(void)arg;
	while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
		ove_shell_process_line("help");
		sleep_ms(5);
	}
	return NULL;
}

/* The console typing commands while browsers open the terminal, type and
 * leave: the terminal's connection pointer was shared across threads. */
static int case_console_beside_terminal(int unused)
{
	(void)unused;
	quiet_stdout();
	ove_httpd_register_builtin_routes();
	if (ove_shell_init() != OVE_OK || start_server() != OVE_OK)
		return 2;
	pthread_t t;
	pthread_create(&t, NULL, console_loop, NULL);
	int opens = 0;
	uint64_t end = now_ms() + 2000;
	for (int i = 0; now_ms() < end; i++) {
		int ws = ws_open("/ws/shell");
		if (ws < 0) {
			sleep_ms(5);
			continue;
		}
		opens++;
		ws_send_text(ws, "help");
		char buf[256];
		while (ws_recv(ws, buf, sizeof(buf), 5) == 1)
			; /* take what has come, without waiting for the rest */
		if (i & 1)
			cli_abort(ws);
		else
			close(ws);
	}
	__atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
	ove_httpd_stop();
	say("terminal: %d sessions beside the console", opens);
	return opens == 0;
}

/* ---------- Routes and the log while the server runs ---------- */

static int ok_route(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;
	return ove_httpd_resp_json(resp, 200, "{}");
}

#define LATE_ROUTES 8

static void *route_adder(void *arg)
{
	static char paths[LATE_ROUTES][16];
	(void)arg;
	for (int i = 0; i < LATE_ROUTES; i++) {
		(void)snprintf(paths[i], sizeof(paths[i]), "/r%d", i);
		if (ove_httpd_route("GET", paths[i], ok_route) != OVE_OK)
			__atomic_add_fetch(&g_close_count, 100,
					   __ATOMIC_RELAXED); /* flags a failure */
		sleep_ms(50);
	}
	return NULL;
}

static int http_status(const char *path)
{
	int g = cli_connect();
	char req[64];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\n\r\n", path);
	(void)send(g, req, (size_t)n, MSG_NOSIGNAL);
	uint8_t b[64];
	int eof;
	size_t got = cli_read(g, b, sizeof(b) - 1, 1000, &eof);
	close(g);
	b[got] = '\0';
	return got >= 12 ? (int)strtol((char *)b + 9, NULL, 10) : -1;
}

/* Routes added while requests are served: the table had no lock. */
static int case_routes_while_running(int unused)
{
	(void)unused;
	if (start_server() != OVE_OK)
		return 2;
	pthread_t t;
	pthread_create(&t, NULL, route_adder, NULL);
	int gets = 0;
	uint64_t end = now_ms() + 600;
	while (now_ms() < end) {
		char path[16];
		(void)snprintf(path, sizeof(path), "/r%d", gets % LATE_ROUTES);
		(void)http_status(path);
		gets++;
	}
	pthread_join(t, NULL);
	int served = 0;
	for (int i = 0; i < LATE_ROUTES; i++) {
		char path[16];
		(void)snprintf(path, sizeof(path), "/r%d", i);
		served += http_status(path) == 200;
	}
	ove_httpd_stop();
	say("routes: %d requests while adding; %d/%d late routes served after", gets, served,
	    LATE_ROUTES);
	return served != LATE_ROUTES || close_count() != 0;
}

static void *early_logger(void *arg)
{
	(void)arg;
	while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
		OVE_LOG_INF("early");
		sleep_ms(1);
	}
	return NULL;
}

/* Threads logging before the built-in routes created the log ring's mutex
 * raced its unlocked flag (and could unlock a mutex they never took). */
static int case_log_before_routes(int unused)
{
	pthread_t t[3];
	(void)unused;
	quiet_stdout();
	for (int i = 0; i < 3; i++)
		pthread_create(&t[i], NULL, early_logger, NULL);
	sleep_ms(50);
	ove_httpd_register_builtin_routes();
	if (start_server() != OVE_OK)
		return 2;
	int status = http_status("/api/log");
	__atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
	for (int i = 0; i < 3; i++)
		pthread_join(t[i], NULL);
	ove_httpd_stop();
	say("log: /api/log answered %d while threads logged", status);
	return status != 200;
}

/* ---------- Runner ---------- */

/* A port nothing listens on now. */
static int free_port(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = {.sin_family = AF_INET};
	socklen_t len = sizeof(sa);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &len) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	return ntohs(sa.sin_port);
}

/* Runs @p fn(@p arg) in a child process; its exit status (0 = kept the
 * contract), or 128 + the signal that killed it. */
static int run_case(int (*fn)(int), int arg)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		alarm(60);
		g_port = free_port();
		_exit(g_port > 0 ? fn(arg) : 2);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void test_ws_survives_http_request(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_http_beside_ws, 0), 0);
}

static void test_ws_survives_queued_http_request(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_http_beside_ws, 1), 0);
}

static void test_ws_failed_send_calls_on_close(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_failed_send, 0), 0);
}

static void test_ws_failed_broadcast_calls_on_close(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_failed_send, 1), 0);
}

static void test_ws_stop_closes_connections(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_stop, 0), 0);
}

static void test_ws_restart_after_stop(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_stop, 1), 0);
}

static void test_ws_broadcast_while_clients_churn(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_send_while_churning, 0), 0);
}

static void test_ws_log_while_clients_churn(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_send_while_churning, 1), 0);
}

static void test_ws_stalled_client_does_not_stall_server(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_stalled_client, 0), 0);
}

static void test_terminal_shows_only_its_commands(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_shell_output_scoped, 0), 0);
}

static void test_console_beside_terminal(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_console_beside_terminal, 0), 0);
}

static void test_routes_added_while_running(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_routes_while_running, 0), 0);
}

static void test_log_before_builtin_routes(void **state)
{
	(void)state;
	assert_int_equal(run_case(case_log_before_routes, 0), 0);
}

/* An optional argument picks the tests to run by name (cmocka pattern). */
int main(int argc, char **argv)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_ws_survives_http_request),
		cmocka_unit_test(test_ws_survives_queued_http_request),
		cmocka_unit_test(test_ws_failed_send_calls_on_close),
		cmocka_unit_test(test_ws_failed_broadcast_calls_on_close),
		cmocka_unit_test(test_ws_stop_closes_connections),
		cmocka_unit_test(test_ws_restart_after_stop),
		cmocka_unit_test(test_ws_broadcast_while_clients_churn),
		cmocka_unit_test(test_ws_log_while_clients_churn),
		cmocka_unit_test(test_ws_stalled_client_does_not_stall_server),
		cmocka_unit_test(test_terminal_shows_only_its_commands),
		cmocka_unit_test(test_console_beside_terminal),
		cmocka_unit_test(test_routes_added_while_running),
		cmocka_unit_test(test_log_before_builtin_routes),
	};
	if (argc > 1)
		cmocka_set_test_filter(argv[1]);
	return cmocka_run_group_tests_name("httpd_ws", tests, NULL, NULL);
}
