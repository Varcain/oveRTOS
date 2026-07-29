/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * TCP loopback integration test.
 *
 * Stands up an in-process echo server (pthread + raw POSIX sockets) on
 * 127.0.0.1:<ephemeral>, then drives the oveRTOS ove_socket API as a
 * client: connect, send, recv, close. Verifies round-trip payload and
 * exercises the real backend send/recv path — covers a coverage gap
 * where previously only topic-matcher / SHA-1 / Base64 helpers were
 * unit-tested.
 *
 * MQTT publish/subscribe and SNTP round-trips remain Tier 3 follow-ups
 * (they need an external broker / NTP server).
 */

#include "../framework/ove_test.h"
#include "ove/net.h"

#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#ifdef CONFIG_OVE_NET

struct echo_server {
	int listen_fd;
	uint16_t port;
	pthread_t thr;
	int started;
	/* On a failed start: which syscall failed and its errno, so the test
	 * body can tell an environment-denied socket (sandbox) from a real bug. */
	const char *fail_stage;
	int fail_errno;
};

static void *echo_server_thread(void *arg)
{
	struct echo_server *s = (struct echo_server *)arg;
	int conn = accept(s->listen_fd, NULL, NULL);
	if (conn < 0)
		return NULL;

	char buf[128];
	ssize_t n;
	while ((n = recv(conn, buf, sizeof(buf), 0)) > 0) {
		ssize_t sent = 0;
		while (sent < n) {
			ssize_t w = send(conn, buf + sent, (size_t)(n - sent), 0);
			if (w <= 0)
				break;
			sent += w;
		}
	}
	close(conn);
	return NULL;
}

static int echo_server_start(struct echo_server *s)
{
	memset(s, 0, sizeof(*s));
	s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (s->listen_fd < 0) {
		s->fail_stage = "socket";
		s->fail_errno = errno;
		return -1;
	}

	int one = 1;
	setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in sin = {0};
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = 0; /* kernel-assigned */
	if (bind(s->listen_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		s->fail_stage = "bind";
		s->fail_errno = errno;
		close(s->listen_fd);
		return -1;
	}

	socklen_t slen = sizeof(sin);
	if (getsockname(s->listen_fd, (struct sockaddr *)&sin, &slen) < 0) {
		s->fail_stage = "getsockname";
		s->fail_errno = errno;
		close(s->listen_fd);
		return -1;
	}
	s->port = ntohs(sin.sin_port);

	if (listen(s->listen_fd, 1) < 0) {
		s->fail_stage = "listen";
		s->fail_errno = errno;
		close(s->listen_fd);
		return -1;
	}

	/* pthread_create returns the error number directly (does not set errno). */
	int thr_rc = pthread_create(&s->thr, NULL, echo_server_thread, s);
	if (thr_rc != 0) {
		s->fail_stage = "pthread_create";
		s->fail_errno = thr_rc;
		close(s->listen_fd);
		return -1;
	}
	s->started = 1;
	return 0;
}

static void echo_server_stop(struct echo_server *s)
{
	if (s->started) {
		/* The worker may still be blocked in accept() — e.g. the test
		 * aborted (cmocka longjmp on a failed assert) before the client
		 * connected.  Poke the listen port with a throwaway connection so
		 * accept() returns and the thread can exit; otherwise pthread_join
		 * would hang forever.  In the normal path the client's close has
		 * already let the worker exit and this extra connection just sits
		 * unaccepted until we close it. */
		int waker = socket(AF_INET, SOCK_STREAM, 0);
		if (waker >= 0) {
			struct sockaddr_in sin = {0};
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			sin.sin_port = htons(s->port);
			(void)connect(waker, (struct sockaddr *)&sin, sizeof(sin));
			close(waker);
		}
		pthread_join(s->thr, NULL);
		s->started = 0;
	}
	if (s->listen_fd >= 0) {
		close(s->listen_fd);
		s->listen_fd = -1;
	}
}

/* Fixture context: holds the server + client socket so loopback_teardown can
 * reclaim them even when the test body aborts mid-way via a failed assert.
 * A single static instance is fine — cmocka runs the test serially. */
struct loopback_ctx {
	struct echo_server srv;
	int srv_started;
	ove_socket_storage_t sock_storage;
	ove_socket_t sock;
	int sock_open;
};

static struct loopback_ctx g_loopback;

static int loopback_setup(void **state)
{
	memset(&g_loopback, 0, sizeof(g_loopback));
	/* Always succeed setup (so teardown runs) even if the echo server could
	 * not start — the test body inspects srv.fail_* to skip (environment
	 * denied the socket) vs. fail (a real bug) with a clear message, instead
	 * of cmocka reporting an opaque "setup error". */
	g_loopback.srv_started = (echo_server_start(&g_loopback.srv) == 0);
	*state = &g_loopback;
	return 0;
}

static int loopback_teardown(void **state)
{
	struct loopback_ctx *ctx = (struct loopback_ctx *)*state;
	if (!ctx)
		return 0;
	/* Runs even when the test aborts on a failed assert, so the client
	 * socket and the echo server (incl. its pthread) are always reclaimed:
	 * no fd/socket leak and no joinless thread wedging process teardown. */
	if (ctx->sock_open)
		ove_socket_close(ctx->sock);
	if (ctx->srv_started)
		echo_server_stop(&ctx->srv);
	return 0;
}

static void test_tcp_loopback_echo(void **state)
{
	struct loopback_ctx *ctx = (struct loopback_ctx *)*state;

	if (!ctx->srv_started) {
		const int e = ctx->srv.fail_errno;
		/* Restricted sandboxes (seccomp/containers) deny opening a local
		 * TCP socket — that's an environment limitation, not a backend
		 * bug, so skip rather than fail.  Any other failure is real. */
		if (e == EPERM || e == EACCES || e == EAFNOSUPPORT || e == EPROTONOSUPPORT) {
			print_message("[  SKIP  ] net_loopback — local TCP socket denied "
				      "at %s(): %s\n",
				      ctx->srv.fail_stage, strerror(e));
			skip();
		}
		fail_msg("loopback echo server %s() failed: %s", ctx->srv.fail_stage, strerror(e));
	}

	assert_int_equal(ove_socket_open(&ctx->sock, &ctx->sock_storage, OVE_AF_INET,
					 OVE_SOCK_STREAM),
			 OVE_OK);
	ctx->sock_open = 1;

	ove_sockaddr_t dst = {
		.family = OVE_AF_INET,
		.port = ctx->srv.port,
		.addr = {127, 0, 0, 1},
	};
	assert_int_equal(ove_socket_connect(ctx->sock, &dst, OVE_SEC(2)), OVE_OK);

	static const char payload[] = "oveRTOS loopback";
	const size_t plen = sizeof(payload) - 1;
	size_t sent = 0;
	assert_int_equal(ove_socket_send(ctx->sock, payload, plen, &sent), OVE_OK);
	assert_int_equal(sent, plen);

	char rxbuf[64] = {0};
	size_t received = 0;
	assert_int_equal(ove_socket_recv(ctx->sock, rxbuf, sizeof(rxbuf), &received, OVE_SEC(2)),
			 OVE_OK);
	assert_int_equal(received, plen);
	assert_memory_equal(rxbuf, payload, plen);

	/* Socket + server reclaimed by loopback_teardown (runs on success and
	 * on assert-failure alike). */
}

static void test_bind_address_not_available(void **state)
{
	(void)state;
	ove_socket_storage_t storage;
	ove_socket_t sock;
	int open_rc = ove_socket_open(&sock, &storage, OVE_AF_INET, OVE_SOCK_DGRAM);
	if (open_rc == OVE_ERR_NOT_SUPPORTED) {
		print_message("[  SKIP  ] bind address-not-available — local socket denied\n");
		skip();
	}
	assert_int_equal(open_rc, OVE_OK);

	/* TEST-NET-1 is not assigned to a host interface. */
	ove_sockaddr_t addr = {
		.family = OVE_AF_INET,
		.port = 0,
		.addr = {192, 0, 2, 1},
	};
	int rc = ove_socket_bind(sock, &addr);
	ove_socket_close(sock);
	assert_int_equal(rc, OVE_ERR_NET_ADDR_NOT_AVAILABLE);
}

int test_net_loopback_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(test_tcp_loopback_echo, loopback_setup,
						loopback_teardown),
		cmocka_unit_test(test_bind_address_not_available),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

#else /* !CONFIG_OVE_NET */

int test_net_loopback_run(void)
{
	print_message("[  SKIP  ] net_loopback — CONFIG_OVE_NET not enabled\n");
	return 0;
}

#endif /* CONFIG_OVE_NET */
