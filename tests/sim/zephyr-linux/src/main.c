/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Step-1 USERSPACE smoke test for the Zephyr Linux-personality bring-up
 * (mps2/an521/cpu0, Cortex-M33). Spawn an unprivileged (K_USER) thread and have
 * it cross into the kernel via a Zephyr syscall (k_sem_give, which traps with
 * svc). The privileged main thread observes the give through a granted
 * semaphore: a successful take proves CONFIG_USERSPACE runs an unprivileged
 * thread and the user->kernel svc path round-trips — the prerequisite for
 * trapping a loaded program's Linux svc.
 *
 * I/O and exit go through ARM semihosting rather than the console UART: the
 * an521 QEMU board routes its UART to a host pty, so semihosting is the
 * reliable way to surface output and shut QEMU down (SYS_EXIT) for the harness.
 * main runs privileged, so the semihosting bkpt is legal.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include "ove/arena.h"
#include "ove/linux/syscall.h"

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

K_THREAD_STACK_DEFINE(user_stack, 2048);
static struct k_thread user_thread;
K_SEM_DEFINE(done_sem, 0, 1);

/*
 * Linux SVC seam (step-2 core): interpose Zephyr's software-fault handler so an
 * unprivileged `svc #0` — which Zephyr treats as an unused/oops svc — is decoded
 * as a Linux syscall instead. Linker --wrap gives us __real_z_do_kernel_oops for
 * the genuine-oops chain. The latch keeps real oopses (outside a Linux run) on
 * the fatal path. After we return, svc.S's exception return resumes the thread
 * with the result we wrote into the stacked r0.
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

/* Linux write(2) via the trap: nr=4 in r7, fd/buf/len in r0/r1/r2. */
static inline long lnx_write(int fd, const void *buf, unsigned int len)
{
	register long r7 __asm__("r7") = OVE_LNX_NR_write;
	register long r0 __asm__("r0") = fd;
	register long r1 __asm__("r1") = (long)buf;
	register long r2 __asm__("r2") = (long)len;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
	return r0;
}

static const char k_msg[] = "hi from svc\n";

static void user_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	/* Unprivileged Linux write(1, ...): traps via svc #0, dispatched by the
	 * seam into ove_lnx_syscall -> our sink. Signal only on the right count. */
	if (lnx_write(1, k_msg, sizeof(k_msg) - 1) == (long)(sizeof(k_msg) - 1)) {
		k_sem_give(&done_sem);
	}
}

static uint8_t s_pool[2048] __aligned(16);

int main(void)
{
	sh_write0("=== Zephyr Linux write(2) personality test (an521) ===\n");

	/* Engine-agnostic process context: arena-backed brk + our fd sink. */
	ove_arena_t arena;
	ove_arena_init(&arena, s_pool, sizeof(s_pool));
	ove_lnx_proc_init(&g_proc, &arena, 512);
	g_proc.write_fn = capture_write;

	g_lnx_active = 1;

	/* Create suspended so we can grant the user thread access to the
	 * semaphore kernel object before it runs. */
	k_tid_t tid = k_thread_create(&user_thread, user_stack, K_THREAD_STACK_SIZEOF(user_stack),
				      user_entry, NULL, NULL, NULL, 5, K_USER, K_FOREVER);
	k_thread_access_grant(tid, &done_sem);
	k_thread_start(tid);

	int ok = (k_sem_take(&done_sem, K_MSEC(1000)) == 0);
	g_lnx_active = 0;

	if (ok && g_cap_len == sizeof(k_msg) - 1 && memcmp(g_cap, k_msg, g_cap_len) == 0) {
		sh_write0("[zephyr-linux] write(1) svc #0 -> ove_lnx_syscall -> sink OK\n");
		sh_write0("\n=== Summary: 0 test group(s) had failures ===\n");
		sh_exit(0);
	}

	sh_write0("[zephyr-linux] FAIL: write not dispatched / output mismatch\n");
	sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
	sh_exit(1);
	return 0;
}
