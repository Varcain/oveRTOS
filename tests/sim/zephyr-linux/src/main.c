/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr Linux-personality on-target test (mps2/an521/cpu0, Cortex-M33): load a
 * REAL Buildroot uClibc-ng static bFLT and run it as an unprivileged (K_USER)
 * thread, trapping its Linux syscalls. The program open()s /etc/motd from the
 * read-only in-memory rootfs, read()s it, and write()s it to stdout — so this
 * exercises the file syscalls (open/read/close) end-to-end on real libc code.
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

#include "loader_hello_image.h" /* ove_test_hello_bflt[], _len */

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
			if (g_proc.exited) {
				/* exit/exit_group: don't resume into uClibc's
				 * post-exit path (it would abort and loop). Park
				 * the thread; main reaps it with the real status. */
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
#define PROG_REGION_SIZE 0x18000u /* 96 KiB: ~63 KiB image + program stack */
K_APPMEM_PARTITION_DEFINE(prog_partition);
K_APP_BMEM(prog_partition) static uint8_t prog_region[PROG_REGION_SIZE] __aligned(32);

static uint8_t s_pool[0x10000] __aligned(16); /* arena: brk + anonymous mmap */

/* A one-file read-only rootfs the program opens as /etc/motd. */
static const uint8_t g_motd[] = "hello from uClibc\n";
static const ove_lnx_file_t g_rootfs[] = {
	{"/etc/motd", g_motd, sizeof(g_motd) - 1},
};

static struct k_mem_partition text_part;
static struct k_mem_partition data_part;

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

#define EXPECT_MSG "hello from uClibc\n"

int main(void)
{
	sh_write0("=== Zephyr uClibc bFLT personality test (an521) ===\n");

	/* Load the real uClibc bFLT (copies text+data, zeroes bss, relocates). */
	ove_flat_t prog;
	if (ove_loader_load_flat(&prog, ove_test_hello_bflt, ove_test_hello_bflt_len, prog_region,
				 sizeof(prog_region)) != OVE_OK) {
		sh_write0("[zephyr-linux] FAIL: bFLT load failed\n");
		sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
		sh_exit(1);
	}

	/* Process context: arena-backed brk + anonymous mmap + the fd sink. */
	ove_arena_t arena;
	ove_arena_init(&arena, s_pool, sizeof(s_pool));
	ove_lnx_proc_init(&g_proc, &arena, 0x8000);
	g_proc.write_fn = capture_write;
	ove_lnx_proc_set_rootfs(&g_proc, g_rootfs, 1);

	/* Build the System V startup stack in the RW tail of the program region
	 * (above the loaded image), and remember the resulting SP. */
	uint8_t *stack_lo = prog_region + prog.region_used;
	size_t stack_sz = sizeof(prog_region) - prog.region_used;
	const char *const argv[] = {"hello", NULL};
	void *sp = ove_lnx_setup_stack(stack_lo, stack_sz, 1, argv, NULL);
	if (!sp) {
		sh_write0("[zephyr-linux] FAIL: startup stack setup failed\n");
		sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
		sh_exit(1);
	}

	g_lnx_active = 1;

	k_tid_t tid = k_thread_create(&prog_thread, tramp_stack, K_THREAD_STACK_SIZEOF(tramp_stack),
				      lnx_trampoline, sp, (void *)prog.entry, NULL, 5, K_USER,
				      K_FOREVER);

	/* W^X split: text RX, data + bss + program stack RW (32-aligned by elf2flt). */
	text_part.start = (uintptr_t)prog_region;
	text_part.size = prog.text_size;
	text_part.attr = K_MEM_PARTITION_P_RX_U_RX;
	data_part.start = (uintptr_t)prog_region + prog.text_size;
	data_part.size = sizeof(prog_region) - prog.text_size;
	data_part.attr = K_MEM_PARTITION_P_RW_U_RW;
	int rc = k_mem_domain_add_partition(&k_mem_domain_default, &text_part);
	rc |= k_mem_domain_add_partition(&k_mem_domain_default, &data_part);
	if (rc != 0) {
		sh_write0("[zephyr-linux] FAIL: program MPU mapping rejected\n");
		sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
		sh_exit(1);
	}

	k_thread_start(tid);

	/* uClibc loops after exit_group; poll the exit latch, then reclaim. */
	int ok = 0;
	for (int i = 0; i < 2000; i++) {
		if (g_proc.exited) {
			ok = 1;
			break;
		}
		k_msleep(1);
	}
	g_lnx_active = 0;
	k_thread_abort(tid);

	if (ok && g_proc.exit_status == 1 && g_cap_len == sizeof(EXPECT_MSG) - 1 &&
	    memcmp(g_cap, EXPECT_MSG, g_cap_len) == 0) {
		sh_write0("[zephyr-linux] uClibc bFLT ran unprivileged: open+read /etc/motd, "
			  "write, exit trapped OK\n");
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
