/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr Linux-personality on-target test (mps2/an521/cpu0, Cortex-M33).
 * End-to-end: load a real elf2flt bFLT, run it as an unprivileged (K_USER)
 * thread inside an MPU memory domain, and trap its Linux syscalls.
 *
 *   bFLT (svc #0, nr in r7)  ->  Zephyr's svc dispatch routes an unprivileged
 *   "unused" svc to z_do_kernel_oops, which linker --wrap reroutes to our seam
 *   ->  ove_lnx_syscall (engine-agnostic core)  ->  fd sink / arena brk.
 *
 * The program write(1,"hi from bFLT\n")s then exit_group(7)s and returns; the
 * thread then terminates, and main verifies the captured output + exit status.
 * Because Zephyr context-switches via PendSV (not svc), returning from the seam
 * resumes the program cleanly — the property NuttX's flat build lacked.
 *
 * Executable loaded code: the load buffer lives in an app-memory partition
 * section (K_APP_BMEM) — MPU-aligned and outside kernel .bss, so it does not
 * overlap the static SRAM region — and the partition is mapped RX (read +
 * execute) for the unprivileged thread. It is RX, not RWX, because Zephyr
 * enforces W^X (CONFIG_EXECUTE_XOR_WRITE) and rejects a writable+executable
 * partition; the loader applied its relocations earlier while privileged, and
 * the program's stack and brk live elsewhere, so the running program only ever
 * reads and executes this region. (A program that writes its own data segment
 * at run time would need a separate RW data partition — a later step.)
 *
 * I/O and exit use ARM semihosting (the an521 QEMU board routes its UART to a
 * host pty); main runs privileged, so the semihosting bkpt is legal.
 */

#include <zephyr/kernel.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <string.h>

#include "ove/arena.h"
#include "ove/linux/syscall.h"
#include "ove/loader.h"

#include "loader_lnx_mod_image.h" /* ove_loader_test_lnx_arm[], _len */

/* ARM semihosting (Armv7-M/Armv8-M): host console + clean QEMU exit. The
 * canonical in/out r0 form ("+r") keeps the op live in r0 across inlining. */
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

static void sh_exit(unsigned int code)
{
	unsigned long block[2] = {0x20026u /* ADP_Stopped_ApplicationExit */, code};
	semihost(0x20 /* SYS_EXIT_EXTENDED */, block);
	for (;;) {
	}
}

K_THREAD_STACK_DEFINE(prog_stack, 2048);
static struct k_thread prog_thread;

/*
 * Linux SVC seam: interpose Zephyr's software-fault handler so an unprivileged
 * `svc #0` — which Zephyr treats as an unused/oops svc — is decoded as a Linux
 * syscall instead. Linker --wrap gives us __real_z_do_kernel_oops for the
 * genuine-oops chain; the latch keeps real oopses (outside a Linux run) on the
 * fatal path. After we return, svc.S's exception return resumes the thread with
 * the result we wrote into the stacked r0.
 */
extern void __real_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
				    uint32_t exc_return);

static volatile int g_lnx_active;
static ove_lnx_proc_t g_proc; /* the running Linux program's context */

void __wrap_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
			     uint32_t exc_return)
{
	if (g_lnx_active) {
		/* The svc immediate is the low byte of the 16-bit svc encoding
		 * (0xDFxx) at pc-2; Linux uses #0. */
		const uint16_t *svc = (const uint16_t *)(esf->basic.pc - 2);
		if ((*svc & 0xff00u) == 0xdf00u && (*svc & 0x00ffu) == 0x00u) {
			/* Linux ABI: nr in r7, args r0..r5. r0..r3 are in the
			 * stacked basic frame; r4/r5/r7 in the callee-saved set. */
			long r = ove_lnx_syscall(&g_proc, (long)callee->v4, (int32_t)esf->basic.r0,
						 (int32_t)esf->basic.r1, (int32_t)esf->basic.r2,
						 (int32_t)esf->basic.r3, (int32_t)callee->v1,
						 (int32_t)callee->v2);
			((struct arch_esf *)esf)->basic.r0 = (uint32_t)r;
			return; /* svc.S resumes the thread with the result in r0 */
		}
	}
	__real_z_do_kernel_oops(esf, callee, exc_return);
}

/* fd 1/2 sink: capture what the program writes so main can verify it. */
static char g_cap[64];
static volatile size_t g_cap_len;

static long capture_write(void *ctx, int fd, const void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	if (g_cap_len + len > sizeof(g_cap)) {
		len = sizeof(g_cap) - g_cap_len;
	}
	memcpy(g_cap + g_cap_len, buf, len);
	g_cap_len += len;
	return (long)len;
}

/*
 * The bFLT load region, in its own app-memory partition: K_APP_BMEM places it
 * in an MPU-aligned _app_smem section (not kernel .bss), and we map the
 * partition RX at runtime so the unprivileged thread can execute the text.
 */
K_APPMEM_PARTITION_DEFINE(prog_partition);
K_APP_BMEM(prog_partition) static uint8_t prog_region[2048];

static uint8_t s_pool[2048] __aligned(16); /* arena for the program break */

#define EXPECT_MSG "hi from bFLT\n"

int main(void)
{
	sh_write0("=== Zephyr Linux bFLT personality test (an521) ===\n");

	/* Load the bFLT into the (MPU-backed, executable) program region. */
	ove_flat_t prog;
	if (ove_loader_load_flat(&prog, ove_loader_test_lnx_arm, ove_loader_test_lnx_arm_len,
				 prog_region, sizeof(prog_region)) != OVE_OK) {
		sh_write0("[zephyr-linux] FAIL: bFLT load failed\n");
		sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
		sh_exit(1);
	}

	/* Engine-agnostic process context: arena-backed brk + our fd sink. */
	ove_arena_t arena;
	ove_arena_init(&arena, s_pool, sizeof(s_pool));
	ove_lnx_proc_init(&g_proc, &arena, 512);
	g_proc.write_fn = capture_write;

	g_lnx_active = 1;

	/* Unprivileged thread entered at the bFLT entry, created suspended so we
	 * can map the program region (RX) into its domain before it runs. */
	k_tid_t tid = k_thread_create(&prog_thread, prog_stack, K_THREAD_STACK_SIZEOF(prog_stack),
				      (k_thread_entry_t)prog.entry, NULL, NULL, NULL, 5, K_USER,
				      K_FOREVER);

	/* Map the program partition RX (W^X forbids RWX), then add it to the
	 * thread's (default) domain, which already carries the libc/TLS partition. */
	prog_partition.attr = K_MEM_PARTITION_P_RX_U_RX;
	int rc = k_mem_domain_add_partition(&k_mem_domain_default, &prog_partition);
	if (rc != 0) {
		sh_write0("[zephyr-linux] FAIL: could not map program region executable\n");
		sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
		sh_exit(1);
	}

	k_thread_start(tid);

	/* The program writes, exit_group(7)s, and returns — terminating itself. */
	int joined = (k_thread_join(tid, K_MSEC(1000)) == 0);
	g_lnx_active = 0;

	if (joined && g_proc.exited && g_proc.exit_status == 7 &&
	    g_cap_len == sizeof(EXPECT_MSG) - 1 && memcmp(g_cap, EXPECT_MSG, g_cap_len) == 0) {
		sh_write0(
			"[zephyr-linux] bFLT ran unprivileged: write + exit_group(7) trapped OK\n");
		sh_write0("\n=== Summary: 0 test group(s) had failures ===\n");
		sh_exit(0);
	}

	sh_write0("[zephyr-linux] FAIL: bFLT run/dispatch/exit mismatch\n");
	sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
	sh_exit(1);
	return 0;
}
