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
	if (s->listen_fd < 0)
		return -1;

	int one = 1;
	setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in sin = {0};
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = 0; /* kernel-assigned */
	if (bind(s->listen_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		close(s->listen_fd);
		return -1;
	}

	socklen_t slen = sizeof(sin);
	if (getsockname(s->listen_fd, (struct sockaddr *)&sin, &slen) < 0) {
		close(s->listen_fd);
		return -1;
	}
	s->port = ntohs(sin.sin_port);

	if (listen(s->listen_fd, 1) < 0) {
		close(s->listen_fd);
		return -1;
	}

	if (pthread_create(&s->thr, NULL, echo_server_thread, s) != 0) {
		close(s->listen_fd);
		return -1;
	}
	s->started = 1;
	return 0;
}

static void echo_server_stop(struct echo_server *s)
{
	if (s->started)
		pthread_join(s->thr, NULL);
	if (s->listen_fd >= 0)
		close(s->listen_fd);
}

static void test_tcp_loopback_echo(void **state)
{
	(void)state;

	struct echo_server srv;
	assert_int_equal(echo_server_start(&srv), 0);

	ove_socket_storage_t sock_storage;
	ove_socket_t sock;
	assert_int_equal(ove_socket_open(&sock, &sock_storage, OVE_AF_INET, OVE_SOCK_STREAM),
			 OVE_OK);

	ove_sockaddr_t dst = {
		.family = OVE_AF_INET,
		.port = srv.port,
		.addr = {127, 0, 0, 1},
	};
	assert_int_equal(ove_socket_connect(sock, &dst, OVE_SEC(2)), OVE_OK);

	static const char payload[] = "oveRTOS loopback";
	const size_t plen = sizeof(payload) - 1;
	size_t sent = 0;
	assert_int_equal(ove_socket_send(sock, payload, plen, &sent), OVE_OK);
	assert_int_equal(sent, plen);

	char rxbuf[64] = {0};
	size_t received = 0;
	assert_int_equal(ove_socket_recv(sock, rxbuf, sizeof(rxbuf), &received, OVE_SEC(2)), OVE_OK);
	assert_int_equal(received, plen);
	assert_memory_equal(rxbuf, payload, plen);

	ove_socket_close(sock);
	echo_server_stop(&srv);
}

int test_net_loopback_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_tcp_loopback_echo),
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
