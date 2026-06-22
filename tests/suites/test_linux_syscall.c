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

	/* A file-backed mapping is refused until there is a VFS. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_mmap2, 0, 256, 0x3, 0, 3, 0),
			 -OVE_LNX_ENOSYS);

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

	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getpid, 0, 0, 0, 0, 0, 0), 1);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getuid32, 0, 0, 0, 0, 0, 0), 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_getegid32, 0, 0, 0, 0, 0, 0), 0);
	/* ioctl(TCGETS) on a non-tty → -ENOTTY, so stdio block-buffers. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_ioctl, 1, 0x5401, 0, 0, 0, 0),
			 -OVE_LNX_ENOTTY);
	/* Thread-bookkeeping stubs succeed so libc startup proceeds. */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_set_tid_address, 0, 0, 0, 0, 0, 0), 1);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_set_robust_list, 0, 0, 0, 0, 0, 0), 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_rt_sigprocmask, 0, 0, 0, 0, 0, 0), 0);
}

static void test_lnx_setup_stack(void **state)
{
	(void)state;
	static uint8_t stk[512] __attribute__((aligned(8)));
	const char *const argv[] = {"/bin/app", "arg1", NULL};
	const char *const envp[] = {"PATH=/bin", "HOME=/", NULL};

	/* uClinux/bFLT layout: sp[0]=argc, sp[1]=argv ptr, sp[2]=envp ptr. */
	uintptr_t *sp = ove_lnx_setup_stack(stk, sizeof(stk), 2, argv, envp);
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
	uintptr_t *sp2 = ove_lnx_setup_stack(stk, sizeof(stk), 1, argv, NULL);
	assert_non_null(sp2);
	assert_int_equal((int)sp2[0], 1);
	char *const *av2 = (char *const *)sp2[1];
	assert_string_equal(av2[0], "/bin/app");
	assert_null((void *)av2[1]); /* argv terminator */
	char *const *ev2 = (char *const *)sp2[2];
	assert_null((void *)ev2[0]); /* empty envp */
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

int test_linux_syscall_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_lnx_write),
		cmocka_unit_test(test_lnx_writev),
		cmocka_unit_test(test_lnx_brk),
		cmocka_unit_test(test_lnx_mmap),
		cmocka_unit_test(test_lnx_init_stubs),
		cmocka_unit_test(test_lnx_setup_stack),
		cmocka_unit_test(test_lnx_exit_and_unknown),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
