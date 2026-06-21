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

void __wrap_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
			     uint32_t exc_return)
{
	if (g_lnx_active) {
		/* The svc immediate is the low byte of the 16-bit svc encoding
		 * (0xDFxx) at pc-2; Linux uses #0. */
		const uint16_t *svc = (const uint16_t *)(esf->basic.pc - 2);
		if ((*svc & 0xff00u) == 0xdf00u && (*svc & 0x00ffu) == 0x00u) {
			long nr = (long)callee->v4; /* r7 = Linux syscall number */
			/* PoC dispatch: echo nr+1000 back in r0. Replaced by
			 * ove_lnx_syscall(nr, r0..r5) in the next step. */
			((struct arch_esf *)esf)->basic.r0 = (uint32_t)(nr + 1000);
			return; /* svc.S resumes the thread with the new r0 */
		}
	}
	__real_z_do_kernel_oops(esf, callee, exc_return);
}

/* Linux-ABI syscall trap: number in r7, result in r0 (the form uClibc emits). */
static inline long lnx_svc(long nr, long a0)
{
	register long r7 __asm__("r7") = nr;
	register long r0 __asm__("r0") = a0;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory");
	return r0;
}

static void user_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	/* Unprivileged Linux-style trap: svc #0 with nr in r7. The seam should
	 * resume us with nr+1000. Only then signal success. */
	if (lnx_svc(42, 0) == 1042) {
		k_sem_give(&done_sem);
	}
}

int main(void)
{
	sh_write0("=== Zephyr Linux-SVC interposition test (an521) ===\n");

	g_lnx_active = 1;

	/* Create suspended so we can grant the user thread access to the
	 * semaphore kernel object before it runs. */
	k_tid_t tid = k_thread_create(&user_thread, user_stack, K_THREAD_STACK_SIZEOF(user_stack),
				      user_entry, NULL, NULL, NULL, 5, K_USER, K_FOREVER);
	k_thread_access_grant(tid, &done_sem);
	k_thread_start(tid);

	int ok = (k_sem_take(&done_sem, K_MSEC(1000)) == 0);
	g_lnx_active = 0;

	if (ok) {
		sh_write0("[zephyr-linux] unprivileged svc #0 trapped + resumed OK\n");
		sh_write0("\n=== Summary: 0 test group(s) had failures ===\n");
		sh_exit(0);
	}

	sh_write0("[zephyr-linux] FAIL: svc #0 not trapped / wrong result\n");
	sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
	sh_exit(1);
	return 0;
}
