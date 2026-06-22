/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr Linux-personality on-target test (mps2/an521/cpu0, Cortex-M33): load a
 * REAL Buildroot uClibc-ng static bFLT and run it as an unprivileged (K_USER)
 * thread, trapping its Linux syscalls. The launcher execve()s /bin/hello2 from
 * the rootfs; main reloads that bFLT, rebuilds the (per-program) MPU domain +
 * stack, and relaunches the thread — exercising the process model's image
 * replacement (execve) end-to-end on real libc code. The program runs in its
 * own k_mem_domain so privileged main (default domain) can always reload it.
 *
 *   uClibc crt0 (svc #0, nr in r7)  ->  Zephyr routes the unprivileged "unused"
 *   svc to z_do_kernel_oops, which linker --wrap reroutes to our seam  ->
 *   ove_lnx_syscall (engine-agnostic core)  ->  fd sink / arena brk+mmap.
 *
 * Startup ABI: the loaded crt0 expects SP -> argc (System V layout). We build
 * that block with ove_lnx_setup_stack() in the program's RW region, then a
 * user-mode trampoline sets SP to it (and r0 = 0, the static fini arg) and
 * branches to the entry.
 *
 * MPU: W^X forbids one RWX region, so the loaded image is split into two
 * partitions over one app-memory buffer — text [0, text_size) RX and the rest
 * (data + bss + the program stack) RW. The split point (data_base) is 32-byte
 * aligned by elf2flt, satisfying the ARMv8-M MPU.
 *
 * I/O + exit go through ARM semihosting (the an521 board's UART is a host pty);
 * main runs privileged so the bkpt is legal. uClibc's _exit loops after
 * exit_group, so we poll proc.exited and abort the thread rather than join.
 */

#include <zephyr/kernel.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <string.h>

#include "ove/arena.h"
#include "ove/linux/syscall.h"
#include "ove/loader.h"

#include "loader_hello_image.h"	 /* ove_test_hello_bflt[], _len  (the launcher) */
#include "loader_hello2_image.h" /* ove_test_hello2_bflt[], _len (execve target) */

/* ARM semihosting (host console + clean QEMU exit). */
static long semihost(unsigned long op, void *arg)
{
	register unsigned long r0 __asm__("r0") = op;
	register void *r1 __asm__("r1") = arg;
	__asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
	return (long)r0;
}

static void sh_write0(const char *s)
{
	semihost(0x04 /* SYS_WRITE0 */, (void *)s);
}

static void sh_write_hex(unsigned long v)
{
	char b[11] = "0x00000000";
	for (int i = 0; i < 8; i++)
		b[2 + i] = "0123456789abcdef"[(v >> ((7 - i) * 4)) & 0xf];
	sh_write0(b);
}

static void sh_exit(unsigned int code)
{
	unsigned long block[2] = {0x20026u /* ADP_Stopped_ApplicationExit */, code};
	semihost(0x20 /* SYS_EXIT_EXTENDED */, block);
	for (;;) {
	}
}

/*
 * Linux SVC seam (linker --wrap, no Zephyr source patch). svc #0 is "Unused" in
 * Zephyr's dispatch, so an unprivileged one reaches z_do_kernel_oops; we decode
 * it as a Linux syscall and resume via svc.S's exception return. Real oopses
 * chain to __real_z_do_kernel_oops.
 */
extern void __real_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
				    uint32_t exc_return);

static volatile int g_lnx_active;
static volatile long g_last_enosys; /* last unimplemented syscall, for diagnostics */
static long g_trace[96];	    /* syscall-number trace, for diagnostics */
static volatile int g_trace_n;
static ove_lnx_proc_t g_proc;

/* Where the program parks after exit_group until main reaps it (see the seam). */
static void park_loop(void)
{
	for (;;) {
	}
}

void __wrap_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
			     uint32_t exc_return)
{
	if (g_lnx_active) {
		const uint16_t *svc = (const uint16_t *)(esf->basic.pc - 2);
		if ((*svc & 0xff00u) == 0xdf00u && (*svc & 0x00ffu) == 0x00u) {
			long nr = (long)callee->v4; /* r7 */
			if (g_trace_n < (int)(sizeof(g_trace) / sizeof(g_trace[0])))
				g_trace[g_trace_n++] = nr;
			long r = ove_lnx_syscall(&g_proc, nr, (int32_t)esf->basic.r0,
						 (int32_t)esf->basic.r1, (int32_t)esf->basic.r2,
						 (int32_t)esf->basic.r3, (int32_t)callee->v1,
						 (int32_t)callee->v2);
			if (r == -OVE_LNX_ENOSYS)
				g_last_enosys = nr;
			if (g_proc.exited || g_proc.exec_pending) {
				/* exit/exit_group, or execve image replacement: do
				 * NOT resume the old image (it would run past the
				 * syscall and race main). Park; main reaps it (exit)
				 * or relaunches it (execve). */
				((struct arch_esf *)esf)->basic.pc = ((uint32_t)&park_loop) | 1u;
				return;
			}
			((struct arch_esf *)esf)->basic.r0 = (uint32_t)r;
			return;
		}
	}
	__real_z_do_kernel_oops(esf, callee, exc_return);
}

/* fd 1/2 sink. */
static char g_cap[128];
static volatile size_t g_cap_len;

static long capture_write(void *ctx, int fd, const void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	if (g_cap_len + len > sizeof(g_cap))
		len = sizeof(g_cap) - g_cap_len;
	memcpy(g_cap + g_cap_len, buf, len);
	g_cap_len += len;
	return (long)len;
}

/*
 * Program memory, in an app-memory partition section (MPU-aligned, in _app_smem
 * rather than kernel .bss). Holds the loaded text+data+bss followed by the
 * program's stack; carved into RX (text) and RW (the rest) partitions below.
 */
/* 192 KiB: ~63 KiB image, then a 64 KiB arena (brk + mmap) and the program's
 * stack — all of which must sit in the program's RW MPU partition so the
 * unprivileged program can touch its heap and stack. */
#define PROG_REGION_SIZE 0x30000u
#define PROG_ARENA_SIZE 0x10000u
K_APPMEM_PARTITION_DEFINE(prog_partition);
K_APP_BMEM(prog_partition) static uint8_t prog_region[PROG_REGION_SIZE] __aligned(32);

/* A small read-only rootfs: /etc/motd plus /bin/hello2, the execve target. */
static const uint8_t g_motd[] = "hello from uClibc\n";
static const ove_lnx_file_t g_rootfs[] = {
	{"/", NULL, 0, OVE_LNX_S_IFDIR},
	{"/etc", NULL, 0, OVE_LNX_S_IFDIR},
	{"/etc/motd", g_motd, sizeof(g_motd) - 1, 0},
	{"/bin", NULL, 0, OVE_LNX_S_IFDIR},
	{"/bin/hello2", ove_test_hello2_bflt, sizeof(ove_test_hello2_bflt), 0},
};
#define G_ROOTFS_N ((int)(sizeof(g_rootfs) / sizeof(g_rootfs[0])))

static struct k_mem_partition text_part;
static struct k_mem_partition data_part;

/*
 * The program runs in its OWN memory domain (program partitions + the libc/heap
 * partitions a user thread needs), NOT the default domain. That leaves the
 * privileged main thread in the unrestricted default domain so it can always
 * (re)load the bFLT into prog_region — needed for execve image replacement.
 */
extern struct k_mem_partition z_libc_partition;
extern struct k_mem_partition z_malloc_partition;
static struct k_mem_domain prog_domain;

/* User-mode trampoline: enter the loaded program with the System V SP and the
 * static-fini r0 the crt0 expects. */
static void lnx_trampoline(void *sp, void *entry, void *unused)
{
	ARG_UNUSED(unused);
	__asm__ volatile("mov sp, %0\n"
			 "mov r0, #0\n"
			 "bx %1\n"
			 :
			 : "r"(sp), "r"(entry)
			 : "r0", "memory");
	__builtin_unreachable();
}

K_THREAD_STACK_DEFINE(tramp_stack, 1024);
static struct k_thread prog_thread;

static k_tid_t g_tid;
static ove_arena_t g_arena; /* persists while the user thread runs */
static int g_parts_added;

/* Captured execve argv, copied out of the process before it is re-initialised. */
static char g_exec_args[OVE_LNX_EXEC_ARGBUF];
static const char *g_exec_ptrs[OVE_LNX_EXEC_MAXARGS + 1];

#define EXPECT_MSG "execed: hello2\n"

/*
 * Load a bFLT into prog_region and launch it as the unprivileged user thread in
 * its own MPU domain. main stays in the default domain, so reloading the image
 * (for the initial program and for each execve replacement) never faults.
 */
static int run_program(const uint8_t *data, size_t len, int argc, const char *const argv[])
{
	/* Tear down the previous image's program partitions (its thread is already
	 * aborted); the libc/heap partitions in prog_domain stay. */
	if (g_parts_added) {
		k_mem_domain_remove_partition(&prog_domain, &text_part);
		k_mem_domain_remove_partition(&prog_domain, &data_part);
	}

	ove_flat_t prog;
	if (ove_loader_load_flat(&prog, data, len, prog_region, sizeof(prog_region)) != OVE_OK)
		return -1;

	uint8_t *rw = prog_region + ((prog.region_used + 15u) & ~15u);
	uint8_t *rw_end = prog_region + sizeof(prog_region);
	ove_arena_init(&g_arena, rw, PROG_ARENA_SIZE);
	ove_lnx_proc_init(&g_proc, &g_arena, 0x8000);
	g_proc.write_fn = capture_write;
	ove_lnx_proc_set_rootfs(&g_proc, g_rootfs, G_ROOTFS_N);

	uint8_t *stack_lo = rw + PROG_ARENA_SIZE;
	void *sp = ove_lnx_setup_stack(stack_lo, (size_t)(rw_end - stack_lo), argc, argv, NULL);
	if (!sp)
		return -1;

	g_tid = k_thread_create(&prog_thread, tramp_stack, K_THREAD_STACK_SIZEOF(tramp_stack),
				lnx_trampoline, sp, (void *)prog.entry, NULL, 5, K_USER, K_FOREVER);

	text_part.start = (uintptr_t)prog_region;
	text_part.size = prog.text_size;
	text_part.attr = K_MEM_PARTITION_P_RX_U_RX;
	data_part.start = (uintptr_t)prog_region + prog.text_size;
	data_part.size = sizeof(prog_region) - prog.text_size;
	data_part.attr = K_MEM_PARTITION_P_RW_U_RW;
	if (!g_parts_added) {
		struct k_mem_partition *base[] = {&z_libc_partition, &z_malloc_partition};
		if (k_mem_domain_init(&prog_domain, 2, base) != 0)
			return -1;
	}
	if (k_mem_domain_add_partition(&prog_domain, &text_part) != 0 ||
	    k_mem_domain_add_partition(&prog_domain, &data_part) != 0)
		return -1;
	g_parts_added = 1;
	k_mem_domain_add_thread(&prog_domain, g_tid);

	k_thread_start(g_tid);
	return 0;
}

int main(void)
{
	sh_write0("=== Zephyr uClibc execve personality test (an521) ===\n");

	g_lnx_active = 1;
	const char *const init_argv[] = {"launcher", NULL};
	if (run_program(ove_test_hello_bflt, ove_test_hello_bflt_len, 1, init_argv) != 0) {
		sh_write0("[zephyr-linux] FAIL: initial program launch failed\n");
		sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
		sh_exit(1);
	}

	/* Drive the program(s): service execve image replacements, stop on exit. */
	int ok = 0;
	for (int i = 0; i < 4000; i++) {
		if (g_proc.exec_pending) {
			/* Copy the captured argv out before re-init clobbers the proc. */
			int idx = g_proc.exec_file_idx;
			int eargc = g_proc.exec_argc;
			size_t off = 0;
			for (int j = 0; j < eargc; j++) {
				size_t n = strlen(g_proc.exec_argv[j]) + 1;
				memcpy(g_exec_args + off, g_proc.exec_argv[j], n);
				g_exec_ptrs[j] = g_exec_args + off;
				off += n;
			}
			g_exec_ptrs[eargc] = NULL;
			const uint8_t *ed = g_rootfs[idx].data;
			size_t el = g_rootfs[idx].size;
			k_thread_abort(g_tid);
			if (run_program(ed, el, eargc, g_exec_ptrs) != 0) {
				sh_write0("[zephyr-linux] FAIL: execve relaunch failed\n");
				sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
				sh_exit(1);
			}
			continue;
		}
		if (g_proc.exited) {
			ok = 1;
			break;
		}
		k_msleep(1);
	}
	g_lnx_active = 0;
	k_thread_abort(g_tid);

	if (ok && g_proc.exit_status == 2 && g_cap_len == sizeof(EXPECT_MSG) - 1 &&
	    memcmp(g_cap, EXPECT_MSG, g_cap_len) == 0) {
		sh_write0("[zephyr-linux] launcher execve(/bin/hello2) replaced the image + ran it "
			  "OK\n");
		sh_write0("\n=== Summary: 0 test group(s) had failures ===\n");
		sh_exit(0);
	}

	sh_write0("[zephyr-linux] FAIL: exited=");
	sh_write_hex((unsigned)g_proc.exited);
	sh_write0(" status=");
	sh_write_hex((unsigned)g_proc.exit_status);
	sh_write0(" caplen=");
	sh_write_hex((unsigned)g_cap_len);
	sh_write0(" last_enosys=");
	sh_write_hex((unsigned long)g_last_enosys);
	sh_write0("\n  trace:");
	for (int t = 0; t < g_trace_n; t++) {
		sh_write0(" ");
		sh_write_hex((unsigned long)g_trace[t]);
	}
	sh_write0("\n");
	if (g_cap_len) {
		sh_write0("  out=");
		sh_write0(g_cap);
		sh_write0("\n");
	}
	sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
	sh_exit(1);
	return 0;
}
