/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux personality syscall-dispatch tests: drive ove_lnx_syscall() directly
 * (no hardware SVC) against an arena-backed process context, checking the
 * minimal Phase-A syscall set translates to the right oveRTOS behaviour.
 */

#include "../framework/ove_test.h"
#include "ove/arena.h"
#include "ove/linux/syscall.h"

#include <stdio.h>
#include <string.h>

/* Captures fd 1/2 output so writes can be asserted by value. */
static char g_cap[256];
static size_t g_cap_len;

static long cap_write(void *ctx, int fd, const void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	if (g_cap_len + len > sizeof(g_cap))
		len = sizeof(g_cap) - g_cap_len;
	memcpy(g_cap + g_cap_len, buf, len);
	g_cap_len += len;
	return (long)len;
}

static uint8_t g_pool[8192] __attribute__((aligned(16)));

static void setup_proc(ove_lnx_proc_t *p, ove_arena_t *arena)
{
	assert_int_equal(ove_arena_init(arena, g_pool, sizeof(g_pool)), OVE_OK);
	assert_int_equal(ove_lnx_proc_init(p, arena, 4096), OVE_OK);
	p->write_fn = cap_write;
	g_cap_len = 0;
}

static void test_lnx_write(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);

	long r = ove_lnx_syscall(&p, OVE_LNX_NR_write, 1, (long)(uintptr_t) "hello", 5, 0, 0, 0);
	assert_int_equal(r, 5);
	assert_int_equal((int)g_cap_len, 5);
	assert_memory_equal(g_cap, "hello", 5);

	/* A bad fd is rejected. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_write, 7, (long)(uintptr_t) "x", 1, 0, 0,
					 0),
			 -OVE_LNX_EBADF);
}

static void test_lnx_writev(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);

	ove_lnx_iovec iov[2] = {
		{(void *)"foo", 3},
		{(void *)"bar!", 4},
	};
	long r = ove_lnx_syscall(&p, OVE_LNX_NR_writev, 2, (long)(uintptr_t)iov, 2, 0, 0, 0);
	assert_int_equal(r, 7);
	assert_int_equal((int)g_cap_len, 7);
	assert_memory_equal(g_cap, "foobar!", 7);
}

static void test_lnx_brk(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);

	/* brk(0) reports the current break without moving it. */
	long base = ove_lnx_syscall(&p, OVE_LNX_NR_brk, 0, 0, 0, 0, 0, 0);
	assert_int_equal((uintptr_t)base, p.brk_base);

	/* A valid grow moves the break and returns the new value. */
	long grown = ove_lnx_syscall(&p, OVE_LNX_NR_brk, base + 100, 0, 0, 0, 0, 0);
	assert_int_equal(grown, base + 100);
	assert_int_equal(p.brk_cur, (uintptr_t)base + 100);

	/* A request beyond the arena reservation leaves the break unchanged. */
	long over = ove_lnx_syscall(&p, OVE_LNX_NR_brk, (long)(p.brk_max + 4096), 0, 0, 0, 0, 0);
	assert_int_equal(over, base + 100);
}

static void test_lnx_mmap(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);

	/* Anonymous mmap returns usable, zeroed memory from the arena. */
	long m = ove_lnx_syscall(&p, OVE_LNX_NR_mmap2, 0, 256, 0x3 /*PROT_RW*/,
				 OVE_LNX_MAP_ANONYMOUS, -1, 0);
	assert_true(m > 0);
	uint8_t *mem = (uint8_t *)(uintptr_t)m;
	for (int i = 0; i < 256; i++)
		assert_int_equal(mem[i], 0);
	mem[0] = 0xab; /* and it is writable */
	assert_int_equal(mem[0], 0xab);

	/* munmap succeeds (wholesale reclaim at teardown). */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_munmap, m, 256, 0, 0, 0, 0), 0);

	/* A file-backed mapping reads the fd's bytes into the block (ld.so loads .so segments
	 * this way on NOMMU); an unopened fd is rejected. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_mmap2, 0, 256, 0x3, 0, 3, 0),
			 -OVE_LNX_EBADF);

	/* Exhausting the arena yields -ENOMEM rather than a crash. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_mmap2, 0, 1 << 20, 0x3,
					 OVE_LNX_MAP_ANONYMOUS, -1, 0),
			 -OVE_LNX_ENOMEM);
}

static void test_lnx_init_stubs(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);

	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getpid, 0, 0, 0, 0, 0, 0), 1);	/* pid */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getppid, 0, 0, 0, 0, 0, 0), 0); /* ppid */
	/* wait4: -ECHILD with no child; reaps queued zombies oldest-first (FIFO),
	 * so a pipeline's two children both get reaped. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_wait4, -1, 0, 0, 0, 0, 0), -OVE_LNX_ECHILD);
	p.child_pid[0] = 2;
	p.child_status[0] = 7;
	p.child_pid[1] = 3;
	p.child_status[1] = 0;
	p.child_count = 2;
	int wstatus = -1;
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_wait4, -1, (long)(uintptr_t)&wstatus, 0, 0,
					 0, 0),
			 2);
	assert_int_equal(wstatus, 7 << 8); /* first child: WEXITSTATUS == 7 */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_wait4, -1, 0, 0, 0, 0, 0), 3); /* second */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_wait4, -1, 0, 0, 0, 0, 0), -OVE_LNX_ECHILD);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getuid32, 0, 0, 0, 0, 0, 0), 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getegid32, 0, 0, 0, 0, 0, 0), 0);
	/* Console fds are ttys: TCGETS fills a canonical termios (so isatty → the
	 * shell goes interactive); a non-open fd is not a tty. */
	ove_lnx_termios tio;
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_ioctl, 0, OVE_LNX_TCGETS,
					 (long)(uintptr_t)&tio, 0, 0, 0),
			 0);
	assert_true((tio.c_lflag & OVE_LNX_ICANON) != 0);
	assert_true((tio.c_lflag & OVE_LNX_ECHO) != 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_ioctl, 7, OVE_LNX_TCGETS,
					 (long)(uintptr_t)&tio, 0, 0, 0),
			 -OVE_LNX_ENOTTY);
	ove_lnx_winsize ws;
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_ioctl, 1, OVE_LNX_TIOCGWINSZ,
					 (long)(uintptr_t)&ws, 0, 0, 0),
			 0);
	assert_int_equal(ws.ws_col, 80);
	/* fcntl F_DUPFD duplicates stdin to the lowest free fd >= arg; a too-high
	 * arg has no slot (the shell then retries low for its interactive fd). */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_fcntl64, 0, OVE_LNX_F_DUPFD, 3, 0, 0, 0),
			 3);
	/* A too-high arg (the shell asks for >=255) falls back to the lowest free fd. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_fcntl64, 0, OVE_LNX_F_DUPFD, 255, 0, 0, 0),
			 4);
	/* A blocking poll (timeout < 0) reports the console ready; the caller then
	 * blocks in read() for the real byte. */
	ove_lnx_pollfd pfd = {.fd = 0, .events = OVE_LNX_POLLIN, .revents = 0};
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_poll, (long)(uintptr_t)&pfd, 1, -1, 0, 0, 0), 1);
	assert_int_equal(pfd.revents, OVE_LNX_POLLIN);
	/* A short finite poll is a "is input pending right now?" probe (vi's repaint
	 * gate / read_key's ESC-sequence timeout). With no read-ahead we honestly
	 * report not-ready so a lone ESC stays ESC and vi repaints while inserting. */
	pfd.revents = 0;
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_poll, (long)(uintptr_t)&pfd, 1, 50, 0, 0, 0), 0);
	assert_int_equal(pfd.revents, 0);
	/* Thread-bookkeeping stubs succeed so libc startup proceeds. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_set_tid_address, 0, 0, 0, 0, 0, 0), 1);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_set_robust_list, 0, 0, 0, 0, 0, 0), 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_rt_sigprocmask, 0, 0, 0, 0, 0, 0), 0);
	/* gettid == pid (single-threaded); prctl is accepted (inert). */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_gettid, 0, 0, 0, 0, 0, 0), 1);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_prctl, 0, 0, 0, 0, 0, 0), 0);
	/* rt_sigaction records the per-signal disposition (sa_handler@0, sa_restorer@8)
	 * for the engine seam to deliver; the old disposition is reported via oact. */
	uint32_t act[4] = {0x1234, 0, 0x5678, 0}, oact[4] = {0xff, 0, 0xff, 0};
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_rt_sigaction, OVE_LNX_SIGINT,
					 (long)(uintptr_t)act, (long)(uintptr_t)oact, 0, 0, 0),
			 0);
	assert_int_equal((uint32_t)p.sig_handler[OVE_LNX_SIGINT], 0x1234);
	assert_int_equal((uint32_t)p.sig_restorer[OVE_LNX_SIGINT], 0x5678);
	assert_int_equal(oact[0], OVE_LNX_SIG_DFL); /* was unset (default) */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_rt_sigaction, 99, (long)(uintptr_t)act, 0,
					 0, 0, 0),
			 -OVE_LNX_EINVAL);
	/* getcwd writes "/" and returns its length incl. NUL; -ERANGE if too small. */
	char cwd[8] = {0};
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getcwd, (long)(uintptr_t)cwd, sizeof(cwd),
					 0, 0, 0, 0),
			 2);
	assert_string_equal(cwd, "/");
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getcwd, (long)(uintptr_t)cwd, 1, 0, 0, 0,
					 0),
			 -OVE_LNX_ERANGE);
}

static void test_lnx_setup_stack(void **state)
{
	(void)state;
	static uint8_t stk[512] __attribute__((aligned(8)));
	const char *const argv[] = {"/bin/app", "arg1", NULL};
	const char *const envp[] = {"PATH=/bin", "HOME=/", NULL};

	/* uClinux/bFLT layout: sp[0]=argc, sp[1]=argv ptr, sp[2]=envp ptr. */
	uintptr_t *sp = ove_lnx_setup_stack(stk, sizeof(stk), 2, argv, envp, 0, 0, 0, 0, 0);
	assert_non_null(sp);
	assert_int_equal((uintptr_t)sp & 7u, 0); /* SP is 8-aligned */

	assert_int_equal((int)sp[0], 2); /* argc */
	char *const *av = (char *const *)sp[1];
	assert_string_equal(av[0], "/bin/app");
	assert_string_equal(av[1], "arg1");
	assert_null((void *)av[2]); /* argv terminator */
	char *const *ev = (char *const *)sp[2];
	assert_string_equal(ev[0], "PATH=/bin");
	assert_string_equal(ev[1], "HOME=/");
	assert_null((void *)ev[2]); /* envp terminator */

	/* auxv follows the envp array's NULL (envc=2 -> at ev[3]). */
	const uintptr_t *aux = (const uintptr_t *)&ev[3];
	assert_int_equal(aux[0], OVE_LNX_AT_PAGESZ);
	assert_int_equal(aux[1], 4096);
	assert_int_equal(aux[2], OVE_LNX_AT_RANDOM);
	assert_non_null((void *)aux[3]); /* AT_RANDOM points into the stack */
	assert_int_equal(aux[4], OVE_LNX_AT_NULL);

	/* A NULL environment is accepted (empty envp). */
	uintptr_t *sp2 = ove_lnx_setup_stack(stk, sizeof(stk), 1, argv, NULL, 0, 0, 0, 0, 0);
	assert_non_null(sp2);
	assert_int_equal((int)sp2[0], 1);
	char *const *av2 = (char *const *)sp2[1];
	assert_string_equal(av2[0], "/bin/app");
	assert_null((void *)av2[1]); /* argv terminator */
	char *const *ev2 = (char *const *)sp2[2];
	assert_null((void *)ev2[0]); /* empty envp */
}

/* Mirrors the kernel struct stat64 prefix through st_size (offset 48). */
struct test_kstat64 {
	uint64_t st_dev;
	uint8_t __pad0[4];
	uint32_t __st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint8_t __pad3[4];
	int64_t st_size;
	uint8_t __tail[64];
};

static const uint8_t k_motd[] = "Welcome to oveRTOS\n"; /* 19 bytes + NUL */
static const uint8_t k_elf[] = {0x7f, 'E', 'L', 'F'};
static const ove_lnx_file_t k_rootfs[] = {
	{"/", NULL, 0, OVE_LNX_S_IFDIR},
	{"/etc", NULL, 0, OVE_LNX_S_IFDIR},
	{"/etc/motd", k_motd, sizeof(k_motd) - 1, 0},
	{"/bin", NULL, 0, OVE_LNX_S_IFDIR},
	{"/bin/sh", k_elf, sizeof(k_elf), 0},
};
#define K_ROOTFS_N ((int)(sizeof(k_rootfs) / sizeof(k_rootfs[0])))

static void test_lnx_file(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);
	ove_lnx_proc_set_rootfs(&p, k_rootfs, K_ROOTFS_N);

	const long motd_len = (long)(sizeof(k_motd) - 1);

	/* open a rootfs file -> a fresh (>= 3) fd. */
	long fd = ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
				  (long)(uintptr_t) "/etc/motd", OVE_LNX_O_RDONLY, 0, 0, 0);
	assert_true(fd >= 3);

	/* sequential read + short read + EOF. */
	char buf[32];
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)buf, 7, 0, 0, 0),
			 7);
	assert_memory_equal(buf, "Welcome", 7);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)buf, sizeof(buf),
					 0, 0, 0),
			 motd_len - 7);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)buf, sizeof(buf),
					 0, 0, 0),
			 0);

	/* lseek SET / END. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_lseek, fd, 0, OVE_LNX_SEEK_SET, 0, 0, 0),
			 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)buf, 7, 0, 0, 0),
			 7);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_lseek, fd, 0, OVE_LNX_SEEK_END, 0, 0, 0),
			 motd_len);

	/* fstat64: a regular file with the right size. */
	struct test_kstat64 st;
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_fstat64, fd, (long)(uintptr_t)&st, 0, 0, 0, 0), 0);
	assert_int_equal(st.st_mode & 0xf000u, OVE_LNX_S_IFREG);
	assert_int_equal((long)st.st_size, motd_len);

	/* close -> the fd is no longer valid. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0), 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)buf, 1, 0, 0, 0),
			 -OVE_LNX_EBADF);

	/* errors: missing path, write attempt on the read-only fs. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
					 (long)(uintptr_t) "/nope", OVE_LNX_O_RDONLY, 0, 0, 0),
			 -OVE_LNX_ENOENT);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
					 (long)(uintptr_t) "/etc/motd", 1 /* O_WRONLY */, 0, 0, 0),
			 -OVE_LNX_EROFS);

	/* fstat64 on a standard stream reports a character device. */
	struct test_kstat64 st2;
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_fstat64, 1, (long)(uintptr_t)&st2, 0, 0, 0, 0), 0);
	assert_int_equal(st2.st_mode & 0xf000u, OVE_LNX_S_IFCHR);
}

/* Writable tmpfs overlay: O_CREAT makes a file, O_APPEND extends it, reads see it. */
static void test_lnx_tmpfs(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);
	ove_lnx_proc_set_rootfs(&p, k_rootfs, K_ROOTFS_N);

	/* O_CREAT|O_WRONLY|O_TRUNC creates a fresh writable file; write "hello". */
	long fd = ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
				  (long)(uintptr_t) "/tmp/t.txt",
				  OVE_LNX_O_WRONLY | OVE_LNX_O_CREAT | OVE_LNX_O_TRUNC, 0644, 0, 0);
	assert_true(fd >= 3);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_write, fd, (long)(uintptr_t) "hello", 5, 0,
					 0, 0),
			 5);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0), 0);

	/* Re-open O_APPEND: the offset starts at end-of-file, so "!" extends it. */
	fd = ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
			     (long)(uintptr_t) "/tmp/t.txt", OVE_LNX_O_WRONLY | OVE_LNX_O_APPEND, 0,
			     0, 0);
	assert_true(fd >= 3);
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_write, fd, (long)(uintptr_t) "!", 1, 0, 0, 0), 1);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0), 0);

	/* Read it back: "hello!" (a tmpfs file shadows the read-only rootfs). */
	fd = ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
			     (long)(uintptr_t) "/tmp/t.txt", OVE_LNX_O_RDONLY, 0, 0, 0);
	assert_true(fd >= 3);
	char buf[16];
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)buf, sizeof(buf),
					 0, 0, 0),
			 6);
	assert_memory_equal(buf, "hello!", 6);

	/* fstat64: a regular file sized 6. */
	struct test_kstat64 st;
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_fstat64, fd, (long)(uintptr_t)&st, 0, 0, 0, 0), 0);
	assert_int_equal(st.st_mode & 0xf000u, OVE_LNX_S_IFREG);
	assert_int_equal((long)st.st_size, 6);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0), 0);

	/* Reading a non-existent path without O_CREAT does NOT create it. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
					 (long)(uintptr_t) "/tmp/missing", OVE_LNX_O_RDONLY, 0, 0,
					 0),
			 -OVE_LNX_ENOENT);
}

/* Search a getdents64 buffer for an entry by name; returns its d_type or -1. */
static int dirents_find(const uint8_t *buf, long len, const char *name)
{
	long off = 0;
	while (off + 19 <= len) {
		uint16_t reclen;
		memcpy(&reclen, buf + off + 16, sizeof(reclen));
		if (reclen == 0)
			break;
		uint8_t type = buf[off + 18];
		if (strcmp((const char *)(buf + off + 19), name) == 0)
			return type;
		off += reclen;
	}
	return -1;
}

static void test_lnx_getdents(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);
	ove_lnx_proc_set_rootfs(&p, k_rootfs, K_ROOTFS_N);

	uint8_t dbuf[256];

	/* "/etc" lists one regular file: motd. */
	long fd = ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD, (long)(uintptr_t) "/etc",
				  OVE_LNX_O_RDONLY, 0, 0, 0);
	assert_true(fd >= 3);
	long n = ove_lnx_syscall(&p, OVE_LNX_NR_getdents64, fd, (long)(uintptr_t)dbuf, sizeof(dbuf),
				 0, 0, 0);
	assert_true(n > 0);
	assert_int_equal(dirents_find(dbuf, n, "motd"), OVE_LNX_DT_REG);
	/* A second call drains the directory (returns 0). */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getdents64, fd, (long)(uintptr_t)dbuf,
					 sizeof(dbuf), 0, 0, 0),
			 0);
	/* read() on a directory is -EISDIR. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)dbuf, 4, 0, 0,
					 0),
			 -OVE_LNX_EISDIR);
	ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0);

	/* "/" lists the subdirectories etc and bin. */
	fd = ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD, (long)(uintptr_t) "/",
			     OVE_LNX_O_RDONLY, 0, 0, 0);
	n = ove_lnx_syscall(&p, OVE_LNX_NR_getdents64, fd, (long)(uintptr_t)dbuf, sizeof(dbuf), 0,
			    0, 0);
	assert_int_equal(dirents_find(dbuf, n, "etc"), OVE_LNX_DT_DIR);
	assert_int_equal(dirents_find(dbuf, n, "bin"), OVE_LNX_DT_DIR);
	ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0);

	/* getdents64 on a regular file is -ENOTDIR. */
	fd = ove_lnx_syscall(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD, (long)(uintptr_t) "/etc/motd",
			     OVE_LNX_O_RDONLY, 0, 0, 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getdents64, fd, (long)(uintptr_t)dbuf,
					 sizeof(dbuf), 0, 0, 0),
			 -OVE_LNX_ENOTDIR);
}

static void test_lnx_execve(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);
	ove_lnx_proc_set_rootfs(&p, k_rootfs, K_ROOTFS_N);

	/* execve captures the request for the engine: which program + argv. */
	char *const argv[] = {"prog", "x", NULL};
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_execve, (long)(uintptr_t) "/bin/sh",
					 (long)(uintptr_t)argv, 0, 0, 0, 0),
			 0);
	assert_int_equal(p.exec_pending, 1);
	assert_string_equal(k_rootfs[p.exec_file_idx].path, "/bin/sh");
	assert_int_equal(p.exec_argc, 2);
	assert_string_equal(p.exec_argv[0], "prog");
	assert_string_equal(p.exec_argv[1], "x");

	/* A missing path is -ENOENT; exec'ing a directory is -EACCES. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_execve, (long)(uintptr_t) "/nope",
					 (long)(uintptr_t)argv, 0, 0, 0, 0),
			 -OVE_LNX_ENOENT);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_execve, (long)(uintptr_t) "/etc",
					 (long)(uintptr_t)argv, 0, 0, 0, 0),
			 -OVE_LNX_EACCES);
}

static void test_lnx_exit_and_unknown(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);

	assert_int_equal(p.exited, 0);
	ove_lnx_syscall(&p, OVE_LNX_NR_exit_group, 42, 0, 0, 0, 0, 0);
	assert_int_equal(p.exited, 1);
	assert_int_equal(p.exit_status, 42);

	/* Unimplemented syscalls report -ENOSYS rather than crashing. */
	assert_int_equal(ove_lnx_syscall(&p, 999, 0, 0, 0, 0, 0, 0), -OVE_LNX_ENOSYS);
}

/* Emit one newc CPIO entry into buf at off; returns the new offset. */
static size_t cpio_emit(uint8_t *buf, size_t off, const char *name, uint32_t mode, const char *data,
			uint32_t fsize)
{
	uint32_t nsize = (uint32_t)strlen(name) + 1;
	uint32_t f[13] = {1, mode, 0, 0, 1, 0, fsize, 0, 0, 0, 0, nsize, 0};
	char tmp[9];
	memcpy(buf + off, "070701", 6);
	off += 6;
	for (int i = 0; i < 13; i++) {
		snprintf(tmp, sizeof(tmp), "%08x", f[i]);
		memcpy(buf + off, tmp, 8);
		off += 8;
	}
	memcpy(buf + off, name, nsize);
	off += nsize;
	while (off & 3u)
		buf[off++] = 0;
	if (fsize) {
		memcpy(buf + off, data, fsize);
		off += fsize;
		while (off & 3u)
			buf[off++] = 0;
	}
	return off;
}

static void test_lnx_cpio(void **state)
{
	(void)state;
	/* Hand-build a tiny newc CPIO: "/", "/etc", "/etc/motd". */
	static uint8_t cpio[512];
	size_t off = 0;
	off = cpio_emit(cpio, off, ".", OVE_LNX_S_IFDIR | 0755, NULL, 0);
	off = cpio_emit(cpio, off, "etc", OVE_LNX_S_IFDIR | 0755, NULL, 0);
	off = cpio_emit(cpio, off, "etc/motd", OVE_LNX_S_IFREG | 0644, "hi\n", 3);
	off = cpio_emit(cpio, off, "TRAILER!!!", 0, NULL, 0);

	static ove_lnx_file_t tbl[8];
	static char nbuf[256];
	int n = ove_lnx_cpio_to_rootfs(cpio, off, tbl, 8, nbuf, sizeof(nbuf));
	assert_int_equal(n, 3); /* TRAILER stops the parse */
	assert_string_equal(tbl[0].path, "/");
	assert_true((tbl[0].mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR);
	assert_string_equal(tbl[2].path, "/etc/motd");
	assert_int_equal((int)tbl[2].size, 3);
	assert_memory_equal(tbl[2].data, "hi\n", 3);
	assert_true((tbl[2].mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFREG);

	/* The parsed table drives the VFS: open + read the file. */
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup_proc(&p, &arena);
	ove_lnx_proc_set_rootfs(&p, tbl, n);
	long fd =
		ove_lnx_syscall(&p, OVE_LNX_NR_open, (long)(uintptr_t) "/etc/motd", 0, 0, 0, 0, 0);
	assert_true(fd >= 0);
	char b[8] = {0};
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)b, 8, 0, 0, 0),
			 3);
	assert_memory_equal(b, "hi\n", 3);
}

int test_linux_syscall_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_lnx_write),
		cmocka_unit_test(test_lnx_writev),
		cmocka_unit_test(test_lnx_brk),
		cmocka_unit_test(test_lnx_mmap),
		cmocka_unit_test(test_lnx_init_stubs),
		cmocka_unit_test(test_lnx_setup_stack),
		cmocka_unit_test(test_lnx_file),
		cmocka_unit_test(test_lnx_tmpfs),
		cmocka_unit_test(test_lnx_getdents),
		cmocka_unit_test(test_lnx_execve),
		cmocka_unit_test(test_lnx_exit_and_unknown),
		cmocka_unit_test(test_lnx_cpio),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
