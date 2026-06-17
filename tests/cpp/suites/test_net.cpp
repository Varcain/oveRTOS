/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Regression tests for the C++ net wrappers' storage ownership.
 *
 * The C handle returned by ove_socket_open()/ove_socket_accept() points
 * *into* the storage the wrapper hands it.  A value move must therefore keep
 * that storage at a stable address, and accept() must write into the
 * destination wrapper's own storage — never a stack local.  These tests use
 * the moved-to / accepted socket after the source storage would have died, so
 * the old "copy storage, keep stale handle" bug shows up as a use-after-free
 * (caught by ASan) or a failed syscall.
 */

#include "../framework/ove_test.hpp"
#include <ove/net.hpp>

#include <pthread.h>
#include <cstring>
#include <ctime>

#ifndef CONFIG_OVE_ZERO_HEAP
#include <optional>
#endif

namespace
{

void sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&ts, nullptr);
}

#ifndef CONFIG_OVE_ZERO_HEAP

/* Move-construct `b` from `a`, then destroy `a` *before* using `b`.  With the
 * pre-fix bug `b`'s handle pointed into `a`'s (now-freed) storage. */
void test_cpp_net_tcp_move_ctor(void **state)
{
	(void)state;
	std::optional<ove::TcpSocket> b;
	{
		ove::TcpSocket a;
		assert_true(a.is_open());
		b.emplace(std::move(a));
		assert_false(a.is_open());
	} /* `a` destroyed here */

	assert_true(b->is_open());
	/* Drive the fd: connecting to a closed loopback port refuses cleanly,
	 * which proves `b` holds a live socket rather than a dangling handle. */
	auto r = b->connect(ove::Address::ipv4(127, 0, 0, 1, 1), std::chrono::milliseconds{500});
	assert_false(r.has_value());
}

/* Move-assign from a source that is then destroyed, then bind the moved-to
 * socket (a syscall on its fd). */
void test_cpp_net_udp_move_assign(void **state)
{
	(void)state;
	ove::UdpSocket b;
	{
		ove::UdpSocket a;
		b = std::move(a);
		assert_false(a.is_open());
	} /* `a` destroyed here */

	assert_true(b.is_open());
	auto r = b.bind(ove::Address::ipv4(127, 0, 0, 1, 0));
	assert_true(r.has_value());
}

#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ── accept(): exercise the accepted socket on a real loopback link ─────── */

constexpr uint16_t kAcceptPort = 54713;

void *connector_fn(void *)
{
	ove::TcpSocket c;
	for (int i = 0; i < 100; i++) {
		auto r = c.connect(ove::Address::ipv4(127, 0, 0, 1, kAcceptPort),
				   std::chrono::milliseconds{200});
		if (r.has_value())
			break;
		sleep_ms(10);
	}
	const char msg[] = "hi";
	(void)c.send(msg, sizeof(msg));
	sleep_ms(100); /* hold the connection so the server can recv */
	return nullptr;
}

void test_cpp_net_accept_then_use(void **state)
{
	(void)state;
	ove::TcpListener listener;
	assert_true(listener.bind(ove::Address::ipv4(127, 0, 0, 1, kAcceptPort)).has_value());
	assert_true(listener.listen(1).has_value());

	pthread_t th;
	assert_int_equal(pthread_create(&th, nullptr, connector_fn, nullptr), 0);

	ove::TcpSocket client;
	auto ra = listener.accept(client, std::chrono::seconds{3});
	assert_true(ra.has_value());
	assert_true(client.is_open());

	/* Use the accepted socket.  With the pre-fix bug its handle pointed at a
	 * stack local freed when accept() returned — this recv would be a
	 * use-after-free (ASan) or read garbage. */
	char buf[16] = {};
	auto rr = client.recv(buf, sizeof(buf), std::chrono::seconds{3});
	assert_true(rr.has_value());
	assert_true(*rr >= 3);
	assert_int_equal(std::strncmp(buf, "hi", 2), 0);

	pthread_join(th, nullptr);
}

} /* namespace */

int test_cpp_net_run(void)
{
	const struct CMUnitTest tests[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_net_tcp_move_ctor),
		cmocka_unit_test(test_cpp_net_udp_move_assign),
#endif
		cmocka_unit_test(test_cpp_net_accept_then_use),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
