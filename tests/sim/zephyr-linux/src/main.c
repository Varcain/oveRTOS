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

static void user_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	/* Unprivileged: k_sem_give is a Zephyr syscall, entered via svc. */
	k_sem_give(&done_sem);
}

int main(void)
{
	sh_write0("=== Zephyr USERSPACE smoke test (an521) ===\n");

	/* Create suspended so we can grant the user thread access to the
	 * semaphore kernel object before it runs. */
	k_tid_t tid = k_thread_create(&user_thread, user_stack, K_THREAD_STACK_SIZEOF(user_stack),
				      user_entry, NULL, NULL, NULL, 5, K_USER, K_FOREVER);
	k_thread_access_grant(tid, &done_sem);
	k_thread_start(tid);

	if (k_sem_take(&done_sem, K_MSEC(1000)) == 0) {
		sh_write0("[zephyr-linux] user thread svc round-trip OK\n");
		sh_write0("\n=== Summary: 0 test group(s) had failures ===\n");
		sh_exit(0);
	}

	sh_write0("[zephyr-linux] FAIL: unprivileged thread never signalled\n");
	sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
	sh_exit(1);
	return 0;
}
