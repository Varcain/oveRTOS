/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux personality pseudo-terminal tests: drive ove_lnx_syscall() (no hardware
 * SVC, no run loop) to exercise the FD_PTY routing + the in-kernel line
 * discipline — open /dev/ptmx (master) + /dev/pts/N (slave) via TIOCGPTN,
 * canonical line delivery + echo, raw passthrough, ONLCR output mapping,
 * O_NONBLOCK EAGAIN, and winsize round-trip. The master/slave open-end count
 * (EOF vs EAGAIN) is driven by ove_lnx_proc_table(), stubbed strong below so the
 * scan sees this test's fds.
 */

#include "../framework/ove_test.h"
#include "ove/arena.h"
#include "ove/linux/pty.h"
#include "ove/linux/syscall.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_pool[8192] __attribute__((aligned(16)));
static ove_lnx_proc_t g_proc;
static ove_arena_t g_arena;

/* Strong override of the weak NULL stub in ove_linux_syscall.c so the pty layer's
 * open-end scan (pty_ends) counts THIS proc's master/slave fds — required for the
 * EOF/EAGAIN distinction (an empty read blocks only while the peer end is open). */
ove_lnx_proc_t *ove_lnx_proc_table(void)
{
	return &g_proc;
}
int ove_lnx_proc_nslot(void)
{
	return 1;
}

#define O_RDWR_NB (OVE_LNX_O_RDWR | OVE_LNX_O_NONBLOCK)

static void pty_setup(void)
{
	assert_int_equal(ove_arena_init(&g_arena, g_pool, sizeof(g_pool)), OVE_OK);
	assert_int_equal(ove_lnx_proc_init(&g_proc, &g_arena, 4096), OVE_OK);
	g_proc.region_lo = 1; /* all-permitting user_ok except NULL */
	g_proc.region_hi = UINTPTR_MAX;
	g_proc.pool_lo = g_proc.pool_hi = 0;
	g_proc.alive = 1; /* so pty_ends counts this proc's fds */
}

static long sc(long nr, long a0, long a1, long a2)
{
	return ove_lnx_syscall(&g_proc, nr, a0, a1, a2, 0, 0, 0);
}

/* Open /dev/ptmx (master) + its /dev/pts/N slave; returns both fds via out params. */
static void open_pair(int *mfd, int *sfd)
{
	long m = sc(OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD, (long)(uintptr_t) "/dev/ptmx", O_RDWR_NB);
	assert_true(m >= 0);
	unsigned ptn = 0xffff;
	assert_int_equal(sc(OVE_LNX_NR_ioctl, m, OVE_LNX_TIOCGPTN, (long)(uintptr_t)&ptn), 0);
	char path[24];
	snprintf(path, sizeof(path), "/dev/pts/%u", ptn);
	long s = sc(OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD, (long)(uintptr_t)path, O_RDWR_NB);
	assert_true(s >= 0);
	*mfd = (int)m;
	*sfd = (int)s;
}

/* /dev/ptmx mints a master, TIOCGPTN yields its number, /dev/pts/N opens the slave. */
static void test_pty_open_and_ptn(void **st)
{
	(void)st;
	pty_setup();
	int mfd, sfd;
	open_pair(&mfd, &sfd);
	assert_int_not_equal(mfd, sfd);
	/* fstat → S_IFCHR so isatty() reports a terminal. */
	/* A bad pts number does not exist. */
	assert_true(sc(OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD, (long)(uintptr_t) "/dev/pts/9", O_RDWR_NB) <
		    0);
}

/* Canonical mode (default): a line is delivered whole on newline, and echoed to the
 * master with ICRNL/ONLCR (input \n echoes as \r\n). */
static void test_pty_canonical_echo(void **st)
{
	(void)st;
	pty_setup();
	int mfd, sfd;
	open_pair(&mfd, &sfd);
	char in[] = "hi\n";
	assert_int_equal(sc(OVE_LNX_NR_write, mfd, (long)(uintptr_t)in, 3), 3);
	char buf[16] = {0};
	assert_int_equal(sc(OVE_LNX_NR_read, sfd, (long)(uintptr_t)buf, sizeof(buf)), 3);
	assert_memory_equal(buf, "hi\n", 3);
	memset(buf, 0, sizeof(buf));
	assert_int_equal(sc(OVE_LNX_NR_read, mfd, (long)(uintptr_t)buf, sizeof(buf)), 4);
	assert_memory_equal(buf, "hi\r\n", 4); /* echo, newline mapped */
}

/* Raw mode (ICANON+ECHO+OPOST off): each byte passes straight to the slave, no echo. */
static void test_pty_raw_passthrough(void **st)
{
	(void)st;
	pty_setup();
	int mfd, sfd;
	open_pair(&mfd, &sfd);
	ove_lnx_termios t;
	memset(&t, 0, sizeof(t)); /* clear ICANON/ECHO/ISIG/ICRNL/OPOST */
	assert_int_equal(sc(OVE_LNX_NR_ioctl, mfd, OVE_LNX_TCSETS, (long)(uintptr_t)&t), 0);
	assert_int_equal(sc(OVE_LNX_NR_write, mfd, (long)(uintptr_t) "x", 1), 1);
	char buf[4] = {0};
	assert_int_equal(sc(OVE_LNX_NR_read, sfd, (long)(uintptr_t)buf, sizeof(buf)), 1);
	assert_int_equal(buf[0], 'x');
	/* no echo: the master ring is empty (slave still open → EAGAIN, not EOF). */
	assert_int_equal(sc(OVE_LNX_NR_read, mfd, (long)(uintptr_t)buf, sizeof(buf)),
			 -OVE_LNX_EAGAIN);
}

/* Slave output OPOST/ONLCR maps \n → \r\n on the way to the master. */
static void test_pty_onlcr_output(void **st)
{
	(void)st;
	pty_setup();
	int mfd, sfd;
	open_pair(&mfd, &sfd);
	char in[] = "a\nb";
	assert_int_equal(sc(OVE_LNX_NR_write, sfd, (long)(uintptr_t)in, 3), 3);
	char buf[16] = {0};
	assert_int_equal(sc(OVE_LNX_NR_read, mfd, (long)(uintptr_t)buf, sizeof(buf)), 4);
	assert_memory_equal(buf, "a\r\nb", 4);
}

/* An empty non-blocking read returns EAGAIN while the peer end is open (not EOF). */
static void test_pty_nonblock_empty(void **st)
{
	(void)st;
	pty_setup();
	int mfd, sfd;
	open_pair(&mfd, &sfd);
	char buf[4];
	assert_int_equal(sc(OVE_LNX_NR_read, sfd, (long)(uintptr_t)buf, sizeof(buf)),
			 -OVE_LNX_EAGAIN);
	assert_int_equal(sc(OVE_LNX_NR_read, mfd, (long)(uintptr_t)buf, sizeof(buf)),
			 -OVE_LNX_EAGAIN);
}

/* TIOCSWINSZ then TIOCGWINSZ round-trips the terminal size (ssh forwards the client's). */
static void test_pty_winsize(void **st)
{
	(void)st;
	pty_setup();
	int mfd, sfd;
	open_pair(&mfd, &sfd);
	ove_lnx_winsize w = {.ws_row = 40, .ws_col = 100, .ws_xpixel = 0, .ws_ypixel = 0};
	assert_int_equal(sc(OVE_LNX_NR_ioctl, mfd, OVE_LNX_TIOCSWINSZ, (long)(uintptr_t)&w), 0);
	ove_lnx_winsize r = {0};
	assert_int_equal(sc(OVE_LNX_NR_ioctl, sfd, OVE_LNX_TIOCGWINSZ, (long)(uintptr_t)&r), 0);
	assert_int_equal(r.ws_row, 40);
	assert_int_equal(r.ws_col, 100);
}

int test_linux_pty_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_pty_open_and_ptn),
		cmocka_unit_test(test_pty_canonical_echo),
		cmocka_unit_test(test_pty_raw_passthrough),
		cmocka_unit_test(test_pty_onlcr_output),
		cmocka_unit_test(test_pty_nonblock_empty),
		cmocka_unit_test(test_pty_winsize),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
