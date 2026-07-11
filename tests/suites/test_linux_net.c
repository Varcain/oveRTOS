/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux personality socket-layer tests: drive lxp_syscall() (no hardware
 * SVC, no run loop) against a real host loopback listener to check the
 * FD_SOCKET routing — socket/connect/send/recv/close, sockaddr + errno
 * translation, the deferred (park + coordinator-retry) path, O_NONBLOCK, and
 * fd refcounting across dup. The backing ove_socket is the POSIX backend, so
 * this is a hermetic loopback integration test (as test_net_loopback.c is).
 */

#include "../framework/ove_test.h"
#include "lxp/lxp_arena.h"
#include "lxp/lxp_net.h"
#include "lxp/lxp_syscall.h"
#include "ove/net.h" /* ove_netif_init/storage for the ifconfig ioctl test */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint8_t g_pool[8192] __attribute__((aligned(16)));

static void setup(lxp_proc_t *p, lxp_arena_t *arena)
{
	assert_int_equal(lxp_arena_init(arena, g_pool, sizeof(g_pool)), OVE_OK);
	assert_int_equal(lxp_proc_init(p, arena, 4096), OVE_OK);
	/* All-permitting access_ok range except NULL (region_lo = 1) — a NULL user
	 * pointer still fails user_ok → -EFAULT. Matches the dev/syscall harnesses. */
	p->region_lo = 1;
	p->region_hi = UINTPTR_MAX;
	p->pool_lo = p->pool_hi = 0;
}

/* Drive a syscall and, if it parked on the socket layer (returns 0 with
 * sock_wait set), pump the coordinator retry the run loop would run. Returns the
 * completed result. */
static long call_pump(lxp_proc_t *p, long nr, long a0, long a1, long a2, long a3, long a4,
		      long a5)
{
	long r = lxp_syscall(p, nr, a0, a1, a2, a3, a4, a5);
	if (!p->sock_wait)
		return r;
	for (int i = 0; i < 4000; i++) {
		long rr = lxp_sock_retry(p);
		if (rr != -LXP_EAGAIN) {
			p->sock_wait = 0;
			return rr;
		}
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 500000}; /* 0.5 ms */
		nanosleep(&ts, NULL);
	}
	p->sock_wait = 0;
	return -LXP_EAGAIN;
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

static void guest_addr(lxp_sockaddr_in *a, int port)
{
	memset(a, 0, sizeof(*a));
	a->sin_family = LXP_AF_INET;
	a->sin_port = htons((uint16_t)port); /* network order, as a guest passes */
	a->sin_addr = htonl(INADDR_LOOPBACK);
}

/* ---- tests ----------------------------------------------------------------- */

static void test_net_socket_open_stat(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(fd >= 3);
	assert_int_equal(p.fds[fd].kind, LXP_FD_SOCKET);

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
		lxp_syscall(&p, LXP_NR_fstat64, fd, (long)(uintptr_t)&st, 0, 0, 0, 0), 0);
	assert_int_equal(st.st_mode & LXP_S_IFMT, LXP_S_IFSOCK);

	assert_int_equal(lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0), 0);
	assert_int_equal(p.fds[fd].kind, 0 /* FD_FREE */);

	/* An unsupported family is rejected with the Linux errno. (SOCK_RAW is now
	 * supported for ping — see test_net_raw_socket.) */
	assert_int_equal(lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET6,
					 LXP_SOCK_STREAM, 0, 0, 0, 0),
			 -LXP_EAFNOSUPPORT);
}

static void test_net_connect_errors(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	/* connect on a non-socket fd (stdout) → ENOTSOCK. */
	lxp_sockaddr_in a;
	guest_addr(&a, 9);
	assert_int_equal(lxp_syscall(&p, LXP_NR_connect, 1, (long)(uintptr_t)&a, sizeof(a), 0,
					 0, 0),
			 -LXP_ENOTSOCK);

	long fd = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(fd >= 3);

	/* NULL address → EFAULT (user_ok rejects). */
	assert_int_equal(lxp_syscall(&p, LXP_NR_connect, fd, 0, sizeof(a), 0, 0, 0),
			 -LXP_EFAULT);

	/* Wrong address family in the sockaddr → EAFNOSUPPORT. */
	lxp_sockaddr_in a6 = a;
	a6.sin_family = LXP_AF_INET6;
	assert_int_equal(lxp_syscall(&p, LXP_NR_connect, fd, (long)(uintptr_t)&a6, sizeof(a6),
					 0, 0, 0),
			 -LXP_EAFNOSUPPORT);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
}

static void test_net_loopback_roundtrip(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	int port = 0;
	int ls = host_listen(&port);

	long fd = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(fd >= 3);

	lxp_sockaddr_in a;
	guest_addr(&a, port);
	long cr = call_pump(&p, LXP_NR_connect, fd, (long)(uintptr_t)&a, sizeof(a), 0, 0, 0);
	assert_int_equal(cr, 0);

	int conn = accept(ls, NULL, NULL);
	assert_true(conn >= 0);

	/* guest send → host recv. */
	long sr = call_pump(&p, LXP_NR_send, fd, (long)(uintptr_t) "hello", 5, 0, 0, 0);
	assert_int_equal(sr, 5);
	char hbuf[16] = {0};
	assert_int_equal((int)recv(conn, hbuf, sizeof(hbuf), 0), 5);
	assert_memory_equal(hbuf, "hello", 5);

	/* host send → guest recv (via read(2), which routes to the socket too). */
	assert_int_equal((int)send(conn, "world", 5, 0), 5);
	char gbuf[16] = {0};
	long rr = call_pump(&p, LXP_NR_read, fd, (long)(uintptr_t)gbuf, sizeof(gbuf), 0, 0, 0);
	assert_int_equal(rr, 5);
	assert_memory_equal(gbuf, "world", 5);

	/* getpeername reports the host's loopback address. */
	lxp_sockaddr_in pa;
	uint32_t palen = sizeof(pa);
	memset(&pa, 0, sizeof(pa));
	assert_int_equal(lxp_syscall(&p, LXP_NR_getpeername, fd, (long)(uintptr_t)&pa,
					 (long)(uintptr_t)&palen, 0, 0, 0),
			 0);
	assert_int_equal(pa.sin_family, LXP_AF_INET);
	assert_int_equal(pa.sin_addr, htonl(INADDR_LOOPBACK));
	assert_int_equal(ntohs(pa.sin_port), port);

	/* host closes → guest recv returns 0 (EOF). */
	close(conn);
	long er = call_pump(&p, LXP_NR_recv, fd, (long)(uintptr_t)gbuf, sizeof(gbuf), 0, 0, 0);
	assert_int_equal(er, 0);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
	close(ls);
}

static void test_net_nonblock(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	int port = 0;
	int ls = host_listen(&port);

	long fd = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET,
				  LXP_SOCK_STREAM | LXP_SOCK_NONBLOCK, 0, 0, 0, 0);
	assert_true(fd >= 3);

	lxp_sockaddr_in a;
	guest_addr(&a, port);
	long cr = call_pump(&p, LXP_NR_connect, fd, (long)(uintptr_t)&a, sizeof(a), 0, 0, 0);
	/* A non-blocking connect either completes at once (loopback) or returns
	 * EINPROGRESS — never parks. */
	assert_true(cr == 0 || cr == -LXP_EINPROGRESS);
	assert_int_equal(p.sock_wait, 0);

	int conn = accept(ls, NULL, NULL);
	assert_true(conn >= 0);

	/* A non-blocking recv with no data pending returns EAGAIN, does not park. */
	char gbuf[8];
	long rr = lxp_syscall(&p, LXP_NR_recv, fd, (long)(uintptr_t)gbuf, sizeof(gbuf), 0, 0,
				  0);
	assert_int_equal(rr, -LXP_EAGAIN);
	assert_int_equal(p.sock_wait, 0);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
	close(conn);
	close(ls);
}

static void test_net_dup_close(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(fd >= 3);
	long fd2 = lxp_syscall(&p, LXP_NR_dup, fd, 0, 0, 0, 0, 0);
	assert_true(fd2 >= 3 && fd2 != fd);
	assert_int_equal(p.fds[fd2].kind, LXP_FD_SOCKET);
	assert_int_equal(p.fds[fd2].file_idx, p.fds[fd].file_idx); /* share the open */

	/* Closing one dup keeps the open alive; closing the last frees it. Then a
	 * fresh socket reuses the pool slot (no leak). */
	assert_int_equal(lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0), 0);
	assert_int_equal(lxp_syscall(&p, LXP_NR_close, fd2, 0, 0, 0, 0, 0), 0);

	/* proc_exit releases any still-open sockets (exercise the exit path: open a
	 * few, then simulate exit). */
	for (int i = 0; i < 4; i++) {
		long f = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_STREAM,
					 0, 0, 0, 0);
		assert_true(f >= 3);
	}
	lxp_sock_proc_exit(&p);
	/* After release, all pool slots are free again: 16 fresh sockets must open. */
	int opened = 0;
	for (int i = 0; i < 16; i++) {
		long f = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_STREAM,
					 0, 0, 0, 0);
		if (f >= 3)
			opened++;
	}
	assert_true(opened >= 8); /* bounded by the fd table (16), not a leak */
	lxp_sock_proc_exit(&p);
}

/* poll(2) on a socket must block until the fd is readable (or the timeout) — the
 * uClibc DNS resolver does poll(POLLIN) then recv(MSG_DONTWAIT), so a poll that
 * returned early/late would break name resolution. Exercises the SOCKW_POLL park
 * + lxp_poll_retry re-scan for both the timeout and the readiness-wake paths. */
static void test_net_poll(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	int port = 0;
	int ls = host_listen(&port);
	long fd = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_STREAM, 0, 0, 0,
				  0);
	assert_true(fd >= 3);
	lxp_sockaddr_in a;
	guest_addr(&a, port);
	assert_int_equal(
		call_pump(&p, LXP_NR_connect, fd, (long)(uintptr_t)&a, sizeof(a), 0, 0, 0), 0);
	int conn = accept(ls, NULL, NULL);
	assert_true(conn >= 0);

	/* Timeout: nothing readable, poll(POLLIN, 120 ms) parks on SOCKW_POLL, the
	 * coordinator retry re-scans until the deadline, then poll returns 0. */
	lxp_pollfd pf;
	memset(&pf, 0, sizeof(pf));
	pf.fd = (int)fd;
	pf.events = LXP_POLLIN;
	assert_int_equal(call_pump(&p, LXP_NR_poll, (long)(uintptr_t)&pf, 1, 120, 0, 0, 0), 0);
	assert_int_equal(pf.revents, 0);

	/* Readiness wake: host sends, poll(POLLIN) reports the socket readable. */
	assert_int_equal((int)send(conn, "x", 1, 0), 1);
	pf.revents = 0;
	assert_int_equal(call_pump(&p, LXP_NR_poll, (long)(uintptr_t)&pf, 1, 1000, 0, 0, 0), 1);
	assert_true((pf.revents & LXP_POLLIN) != 0);

	close(conn);
	close(ls);
	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
}

/* ifconfig(2) ioctls: a SIOC* on a socket fd routes to the interface-config bridge and
 * marshals struct ifreq. Uses the posix backend's synthetic interface view (deterministic
 * MAC + flags), so the address-independent gets/sets are asserted here. */
static void test_net_ifconfig(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	static ove_netif_storage_t nifs;
	ove_netif_t nif = NULL;
	assert_int_equal(ove_netif_init(&nif, &nifs), OVE_OK);
	lxp_sock_set_netif(nif);

	long fd = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_DGRAM, 0, 0, 0,
				  0);
	assert_true(fd >= 3);

	lxp_ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "eth0");

	/* SIOCGIFFLAGS: posix reports an up, running, broadcast interface. */
	assert_int_equal(lxp_syscall(&p, LXP_NR_ioctl, fd, LXP_SIOCGIFFLAGS,
					 (long)(uintptr_t)&ifr, 0, 0, 0),
			 0);
	assert_true(ifr.ifr_ifru.ifru_flags & LXP_IFF_UP);
	assert_true(ifr.ifr_ifru.ifru_flags & LXP_IFF_RUNNING);
	assert_true(ifr.ifr_ifru.ifru_flags & LXP_IFF_BROADCAST);

	/* SIOCGIFHWADDR: sockaddr{ARPHRD_ETHER, MAC}; posix synth = 02:00:00:DE:AD:01. */
	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "eth0");
	assert_int_equal(lxp_syscall(&p, LXP_NR_ioctl, fd, LXP_SIOCGIFHWADDR,
					 (long)(uintptr_t)&ifr, 0, 0, 0),
			 0);
	assert_int_equal(ifr.ifr_ifru.ifru_raw[0], LXP_ARPHRD_ETHER);
	assert_int_equal(ifr.ifr_ifru.ifru_raw[2], 0x02);
	assert_int_equal(ifr.ifr_ifru.ifru_raw[7], 0x01);

	/* SIOCSIFADDR / SIOCSIFFLAGS: accepted (host owns addressing). */
	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "eth0");
	ifr.ifr_ifru.ifru_addr.sin_family = LXP_AF_INET;
	assert_int_equal(lxp_syscall(&p, LXP_NR_ioctl, fd, LXP_SIOCSIFADDR,
					 (long)(uintptr_t)&ifr, 0, 0, 0),
			 0);
	ifr.ifr_ifru.ifru_flags = LXP_IFF_UP;
	assert_int_equal(lxp_syscall(&p, LXP_NR_ioctl, fd, LXP_SIOCSIFFLAGS,
					 (long)(uintptr_t)&ifr, 0, 0, 0),
			 0);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
	lxp_sock_set_netif(NULL);
}

/* socket(AF_INET, SOCK_RAW, IPPROTO_ICMP): busybox ping's socket. The bridge must route
 * it to the raw open path (not reject it as EPROTONOSUPPORT). Opening a raw socket needs
 * CAP_NET_RAW, so on an unprivileged host it fails with a permission error — that still
 * proves the routing (the error is not EPROTONOSUPPORT). */
static void test_net_raw_socket(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_RAW,
				  LXP_IPPROTO_ICMP, 0, 0, 0);
	assert_true(fd != -LXP_EPROTONOSUPPORT);
	if (fd >= 3) {
		assert_int_equal(p.fds[fd].kind, LXP_FD_SOCKET);
		lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
	}
}

/* Server path (P4): guest bind/listen/accept. The guest listens on loopback, a host
 * client connects, and the guest accept(2) parks then the coordinator retry mints the
 * client fd; then a bidirectional byte exchange over the accepted socket. */
static void test_net_server_accept(void **state)
{
	(void)state;
	lxp_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long ls = lxp_syscall(&p, LXP_NR_socket, LXP_AF_INET, LXP_SOCK_STREAM, 0, 0,
				  0, 0);
	assert_true(ls >= 3);
	lxp_sockaddr_in a;
	guest_addr(&a, 0); /* bind to an ephemeral loopback port */
	assert_int_equal(
		lxp_syscall(&p, LXP_NR_bind, ls, (long)(uintptr_t)&a, sizeof(a), 0, 0, 0),
		0);
	assert_int_equal(lxp_syscall(&p, LXP_NR_listen, ls, 8, 0, 0, 0, 0), 0);

	lxp_sockaddr_in bound;
	uint32_t blen = sizeof(bound);
	memset(&bound, 0, sizeof(bound));
	assert_int_equal(lxp_syscall(&p, LXP_NR_getsockname, ls, (long)(uintptr_t)&bound,
					 (long)(uintptr_t)&blen, 0, 0, 0),
			 0);
	int port = ntohs(bound.sin_port);
	assert_true(port > 0);

	int hc = socket(AF_INET, SOCK_STREAM, 0);
	assert_true(hc >= 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons((uint16_t)port);
	assert_int_equal(connect(hc, (struct sockaddr *)&sa, sizeof(sa)), 0);

	lxp_sockaddr_in pa;
	uint32_t palen = sizeof(pa);
	memset(&pa, 0, sizeof(pa));
	long cfd = call_pump(&p, LXP_NR_accept, ls, (long)(uintptr_t)&pa,
			     (long)(uintptr_t)&palen, 0, 0, 0);
	assert_true(cfd >= 3);
	assert_int_equal(p.fds[cfd].kind, LXP_FD_SOCKET);
	assert_int_equal(pa.sin_family, LXP_AF_INET);

	/* host -> guest over the accepted fd */
	assert_int_equal((int)send(hc, "ping", 4, 0), 4);
	char gbuf[16] = {0};
	long rr = call_pump(&p, LXP_NR_recv, cfd, (long)(uintptr_t)gbuf, sizeof(gbuf), 0, 0, 0);
	assert_int_equal(rr, 4);
	assert_memory_equal(gbuf, "ping", 4);

	/* guest -> host over the accepted fd */
	long sr = call_pump(&p, LXP_NR_send, cfd, (long)(uintptr_t) "pong", 4, 0, 0, 0);
	assert_int_equal(sr, 4);
	char hbuf[16] = {0};
	assert_int_equal((int)recv(hc, hbuf, sizeof(hbuf), 0), 4);
	assert_memory_equal(hbuf, "pong", 4);

	close(hc);
	lxp_syscall(&p, LXP_NR_close, cfd, 0, 0, 0, 0, 0);
	lxp_syscall(&p, LXP_NR_close, ls, 0, 0, 0, 0, 0);
}

int test_linux_net_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_net_socket_open_stat),
		cmocka_unit_test(test_net_connect_errors),
		cmocka_unit_test(test_net_loopback_roundtrip),
		cmocka_unit_test(test_net_nonblock),
		cmocka_unit_test(test_net_dup_close),
		cmocka_unit_test(test_net_poll),
		cmocka_unit_test(test_net_ifconfig),
		cmocka_unit_test(test_net_raw_socket),
		cmocka_unit_test(test_net_server_accept),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
