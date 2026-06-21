/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * On-target Linux-personality test: load a real elf2flt bFLT that issues Linux
 * syscalls via `svc #0`, run it under the NuttX SVC trap, and check the trap
 * dispatched correctly — the program's write(1, ...) reaches our I/O sink and
 * exit_group(7) returns control with the right status. End-to-end proof of the
 * loader + syscall dispatch + SVC trap on the Cortex-M. Dispatched only from
 * the NuttX runner.
 */

#include "framework/ove_test.h"
#include "ove/arena.h"
#include "ove/linux/syscall.h"
#include "ove_lnx_trap.h"

#include <string.h>

#include "loader_lnx_mod_image.h" /* ove_loader_test_lnx_arm[], _len */

/* Captures the program's fd-1 output. */
static char g_out[64];
static size_t g_out_len;

static long capture_write(void *ctx, int fd, const void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	if (g_out_len + len > sizeof(g_out))
		len = sizeof(g_out) - g_out_len;
	memcpy(g_out + g_out_len, buf, len);
	g_out_len += len;
	return (long)len;
}

static uint8_t s_region[1024] __attribute__((aligned(8)));
static uint8_t s_pool[2048] __attribute__((aligned(16)));

static void test_linux_spawn_exec(void **state)
{
	(void)state;

	/* Load the program image. */
	ove_flat_t prog;
	assert_int_equal(ove_loader_load_flat(&prog, ove_loader_test_lnx_arm,
					      ove_loader_test_lnx_arm_len, s_region,
					      sizeof(s_region)),
			 OVE_OK);

	/* Build a process context: arena-backed brk + a capturing stdout sink. */
	ove_arena_t arena;
	assert_int_equal(ove_arena_init(&arena, s_pool, sizeof(s_pool)), OVE_OK);
	ove_lnx_proc_t proc;
	assert_int_equal(ove_lnx_proc_init(&proc, &arena, 1024), OVE_OK);
	proc.write_fn = capture_write;
	g_out_len = 0;

	/* Run it under the SVC trap. The program does write(1,"hi from bFLT\n")
	 * then exit_group(7); both arrive as svc #0 and are dispatched here. */
	assert_int_equal(ove_lnx_run(&proc, &prog), OVE_OK);

	assert_int_equal(proc.exited, 1);
	assert_int_equal(proc.exit_status, 7);
	assert_int_equal((int)g_out_len, 13);
	assert_memory_equal(g_out, "hi from bFLT\n", 13);
}

int test_linux_target_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_linux_spawn_exec),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
