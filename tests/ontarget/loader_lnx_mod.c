/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Freestanding program for the on-target Linux-personality test: no libc, it
 * makes Linux syscalls directly via the ARM-EABI trap (svc #0, number in r7,
 * args in r0..r5, result in r0). Loaded as a bFLT and entered under the SVC
 * trap, lnx_start() writes a relocated string to fd 1 then exit_group(7) — so a
 * correct run proves the trap intercepts svc, the dispatcher reaches the I/O
 * sink, and exit returns control to the supervisor. Built into a bFLT with
 * elf2flt; see the regeneration note in loader_lnx_mod_image.h.
 */

static long lnx_syscall3(long nr, long a0, long a1, long a2)
{
	register long r7 __asm__("r7") = nr;
	register long r0 __asm__("r0") = a0;
	register long r1 __asm__("r1") = a1;
	register long r2 __asm__("r2") = a2;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
	return r0;
}

/* String lives in .rodata/.data; its address is materialised via a relocated
 * literal, so writing it also exercises a base relocation. */
static const char msg[] = "hi from bFLT\n";

void lnx_start(void)
{
	lnx_syscall3(4, 1, (long)msg, sizeof(msg) - 1); /* write(1, msg, 13) */
	lnx_syscall3(248, 7, 0, 0);			/* exit_group(7) */
}
