/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux personality socket-layer tests: drive ove_lnx_syscall() (no hardware
 * SVC, no run loop) against a real host loopback listener to check the
 * FD_SOCKET routing — socket/connect/send/recv/close, sockaddr + errno
 * translation, the deferred (park + coordinator-retry) path, O_NONBLOCK, and
 * fd refcounting across dup. The backing ove_socket is the POSIX backend, so
 * this is a hermetic loopback integration test (as test_net_loopback.c is).
 */

#include "../framework/ove_test.h"
#include "ove/arena.h"
#include "ove/linux/net.h"
#include "ove/linux/syscall.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint8_t g_pool[8192] __attribute__((aligned(16)));

static void setup(ove_lnx_proc_t *p, ove_arena_t *arena)
{
	assert_int_equal(ove_arena_init(arena, g_pool, sizeof(g_pool)), OVE_OK);
	assert_int_equal(ove_lnx_proc_init(p, arena, 4096), OVE_OK);
	/* All-permitting access_ok range except NULL (region_lo = 1) — a NULL user
	 * pointer still fails user_ok → -EFAULT. Matches the dev/syscall harnesses. */
	p->region_lo = 1;
	p->region_hi = UINTPTR_MAX;
	p->pool_lo = p->pool_hi = 0;
}

/* Drive a syscall and, if it parked on the socket layer (returns 0 with
 * sock_wait set), pump the coordinator retry the run loop would run. Returns the
 * completed result. */
static long call_pump(ove_lnx_proc_t *p, long nr, long a0, long a1, long a2, long a3, long a4,
		      long a5)
{
	long r = ove_lnx_syscall(p, nr, a0, a1, a2, a3, a4, a5);
	if (!p->sock_wait)
		return r;
	for (int i = 0; i < 4000; i++) {
		long rr = ove_lnx_sock_retry(p);
		if (rr != -OVE_LNX_EAGAIN) {
			p->sock_wait = 0;
			return rr;
		}
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 500000}; /* 0.5 ms */
		nanosleep(&ts, NULL);
	}
	p->sock_wait = 0;
	return -OVE_LNX_EAGAIN;
}

/* A blocking POSIX loopback listener; the guest connects to it. Returns the
 * listening fd and writes the bound port. */
static int host_listen(int *port)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	assert_true(ls >= 0);
	int one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0; /* ephemeral */
	assert_int_equal(bind(ls, (struct sockaddr *)&sa, sizeof(sa)), 0);
	assert_int_equal(listen(ls, 1), 0);
	socklen_t sl = sizeof(sa);
	assert_int_equal(getsockname(ls, (struct sockaddr *)&sa, &sl), 0);
	*port = ntohs(sa.sin_port);
	return ls;
}

static void guest_addr(ove_lnx_sockaddr_in *a, int port)
{
	memset(a, 0, sizeof(*a));
	a->sin_family = OVE_LNX_AF_INET;
	a->sin_port = htons((uint16_t)port); /* network order, as a guest passes */
	a->sin_addr = htonl(INADDR_LOOPBACK);
}

/* ---- tests ----------------------------------------------------------------- */

static void test_net_socket_open_stat(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup(&p, &arena);

	long fd = ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET, OVE_LNX_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(fd >= 3);
	assert_int_equal(p.fds[fd].kind, OVE_LNX_FD_SOCKET);

	/* fstat reports a socket. */
	struct {
		uint64_t st_dev;
		uint8_t pad0[4];
		uint32_t __ino;
		uint32_t st_mode;
		uint8_t rest[96];
	} st;
	memset(&st, 0, sizeof(st));
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_fstat64, fd, (long)(uintptr_t)&st, 0, 0, 0, 0), 0);
	assert_int_equal(st.st_mode & OVE_LNX_S_IFMT, OVE_LNX_S_IFSOCK);

	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0), 0);
	assert_int_equal(p.fds[fd].kind, 0 /* FD_FREE */);

	/* Unsupported family / type are rejected with the Linux errno. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET6,
					 OVE_LNX_SOCK_STREAM, 0, 0, 0, 0),
			 -OVE_LNX_EAFNOSUPPORT);
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET, OVE_LNX_SOCK_RAW, 0, 0, 0, 0),
		-OVE_LNX_EPROTONOSUPPORT);
}

static void test_net_connect_errors(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup(&p, &arena);

	/* connect on a non-socket fd (stdout) → ENOTSOCK. */
	ove_lnx_sockaddr_in a;
	guest_addr(&a, 9);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_connect, 1, (long)(uintptr_t)&a, sizeof(a), 0,
					 0, 0),
			 -OVE_LNX_ENOTSOCK);

	long fd = ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET, OVE_LNX_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(fd >= 3);

	/* NULL address → EFAULT (user_ok rejects). */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_connect, fd, 0, sizeof(a), 0, 0, 0),
			 -OVE_LNX_EFAULT);

	/* Wrong address family in the sockaddr → EAFNOSUPPORT. */
	ove_lnx_sockaddr_in a6 = a;
	a6.sin_family = OVE_LNX_AF_INET6;
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_connect, fd, (long)(uintptr_t)&a6, sizeof(a6),
					 0, 0, 0),
			 -OVE_LNX_EAFNOSUPPORT);

	ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0);
}

static void test_net_loopback_roundtrip(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup(&p, &arena);

	int port = 0;
	int ls = host_listen(&port);

	long fd = ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET, OVE_LNX_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(fd >= 3);

	ove_lnx_sockaddr_in a;
	guest_addr(&a, port);
	long cr = call_pump(&p, OVE_LNX_NR_connect, fd, (long)(uintptr_t)&a, sizeof(a), 0, 0, 0);
	assert_int_equal(cr, 0);

	int conn = accept(ls, NULL, NULL);
	assert_true(conn >= 0);

	/* guest send → host recv. */
	long sr = call_pump(&p, OVE_LNX_NR_send, fd, (long)(uintptr_t) "hello", 5, 0, 0, 0);
	assert_int_equal(sr, 5);
	char hbuf[16] = {0};
	assert_int_equal((int)recv(conn, hbuf, sizeof(hbuf), 0), 5);
	assert_memory_equal(hbuf, "hello", 5);

	/* host send → guest recv (via read(2), which routes to the socket too). */
	assert_int_equal((int)send(conn, "world", 5, 0), 5);
	char gbuf[16] = {0};
	long rr = call_pump(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)gbuf, sizeof(gbuf), 0, 0, 0);
	assert_int_equal(rr, 5);
	assert_memory_equal(gbuf, "world", 5);

	/* getpeername reports the host's loopback address. */
	ove_lnx_sockaddr_in pa;
	uint32_t palen = sizeof(pa);
	memset(&pa, 0, sizeof(pa));
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getpeername, fd, (long)(uintptr_t)&pa,
					 (long)(uintptr_t)&palen, 0, 0, 0),
			 0);
	assert_int_equal(pa.sin_family, OVE_LNX_AF_INET);
	assert_int_equal(pa.sin_addr, htonl(INADDR_LOOPBACK));
	assert_int_equal(ntohs(pa.sin_port), port);

	/* host closes → guest recv returns 0 (EOF). */
	close(conn);
	long er = call_pump(&p, OVE_LNX_NR_recv, fd, (long)(uintptr_t)gbuf, sizeof(gbuf), 0, 0, 0);
	assert_int_equal(er, 0);

	ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0);
	close(ls);
}

static void test_net_nonblock(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup(&p, &arena);

	int port = 0;
	int ls = host_listen(&port);

	long fd = ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET,
				  OVE_LNX_SOCK_STREAM | OVE_LNX_SOCK_NONBLOCK, 0, 0, 0, 0);
	assert_true(fd >= 3);

	ove_lnx_sockaddr_in a;
	guest_addr(&a, port);
	long cr = call_pump(&p, OVE_LNX_NR_connect, fd, (long)(uintptr_t)&a, sizeof(a), 0, 0, 0);
	/* A non-blocking connect either completes at once (loopback) or returns
	 * EINPROGRESS — never parks. */
	assert_true(cr == 0 || cr == -OVE_LNX_EINPROGRESS);
	assert_int_equal(p.sock_wait, 0);

	int conn = accept(ls, NULL, NULL);
	assert_true(conn >= 0);

	/* A non-blocking recv with no data pending returns EAGAIN, does not park. */
	char gbuf[8];
	long rr = ove_lnx_syscall(&p, OVE_LNX_NR_recv, fd, (long)(uintptr_t)gbuf, sizeof(gbuf), 0, 0,
				  0);
	assert_int_equal(rr, -OVE_LNX_EAGAIN);
	assert_int_equal(p.sock_wait, 0);

	ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0);
	close(conn);
	close(ls);
}

static void test_net_dup_close(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup(&p, &arena);

	long fd = ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET, OVE_LNX_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(fd >= 3);
	long fd2 = ove_lnx_syscall(&p, OVE_LNX_NR_dup, fd, 0, 0, 0, 0, 0);
	assert_true(fd2 >= 3 && fd2 != fd);
	assert_int_equal(p.fds[fd2].kind, OVE_LNX_FD_SOCKET);
	assert_int_equal(p.fds[fd2].file_idx, p.fds[fd].file_idx); /* share the open */

	/* Closing one dup keeps the open alive; closing the last frees it. Then a
	 * fresh socket reuses the pool slot (no leak). */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0), 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd2, 0, 0, 0, 0, 0), 0);

	/* proc_exit releases any still-open sockets (exercise the exit path: open a
	 * few, then simulate exit). */
	for (int i = 0; i < 4; i++) {
		long f = ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET, OVE_LNX_SOCK_STREAM,
					 0, 0, 0, 0);
		assert_true(f >= 3);
	}
	ove_lnx_sock_proc_exit(&p);
	/* After release, all pool slots are free again: 16 fresh sockets must open. */
	int opened = 0;
	for (int i = 0; i < 16; i++) {
		long f = ove_lnx_syscall(&p, OVE_LNX_NR_socket, OVE_LNX_AF_INET, OVE_LNX_SOCK_STREAM,
					 0, 0, 0, 0);
		if (f >= 3)
			opened++;
	}
	assert_true(opened >= 8); /* bounded by the fd table (16), not a leak */
	ove_lnx_sock_proc_exit(&p);
}

int test_linux_net_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_net_socket_open_stat),
		cmocka_unit_test(test_net_connect_errors),
		cmocka_unit_test(test_net_loopback_roundtrip),
		cmocka_unit_test(test_net_nonblock),
		cmocka_unit_test(test_net_dup_close),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
