/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX)

#include "ove/linux/syscall.h"
#include "ove_lnx_trap.h"

#include <nuttx/irq.h> /* irq_attach, xcpt_t; pulls arch/irq.h for REG_* */
#include <setjmp.h>
#include <stdint.h>

/*
 * Linux personality — NuttX / ARMv7-M SVC trap seam.
 *
 * A loaded bFLT program enters the kernel with the Linux ARM-EABI trap
 * `svc #0` (syscall number in r7, args in r0..r5, result in r0). This file is
 * the per-engine seam that catches that trap and hands it to the engine-
 * agnostic dispatcher (ove_lnx_syscall, in linux/ove_linux_syscall.c).
 *
 * Discriminating Linux syscalls from NuttX's own SVCs
 * ---------------------------------------------------
 * NuttX *also* uses `svc #0` — its SYS_syscall immediate is 0x00, with the
 * command passed in r0 — for context switching and syscall return. So the SVC
 * immediate cannot tell a Linux syscall from a NuttX one. We discriminate with
 * a per-run latch (g_active) that is set only while a loaded Linux program is
 * executing: latch set => Linux syscall (decode r7); latch clear => NuttX SVC.
 *
 * Interposition without patching NuttX
 * ------------------------------------
 * SVCall (exception 11) is dispatched through NuttX's normal exception path
 * (exception_common -> arm_doirq -> irq_dispatch -> g_irqvector[]), so the
 * PUBLIC irq_attach() swaps the handler at runtime — no NuttX source change.
 * When the latch is clear we CHAIN to NuttX's own arm_svcall() so scheduling
 * SVCs keep working. That `extern` (declared below) is the *only* NuttX-
 * internal coupling: a reference to an already-linked kernel symbol, not a
 * source patch. NuttX is built unmodified.
 *
 * Returning from exit_group
 * -------------------------
 * exit/exit_group must not resume the program. The handler rewrites the saved
 * exception-return PC to a thread-mode thunk that longjmp()s back into
 * ove_lnx_run() — the same saved-frame rewrite the MPU protected backend uses.
 *
 * PoC limitation
 * --------------
 * The latch only discriminates correctly if the running Linux program issues
 * NO blocking NuttX SVC while active (true for the freestanding write/exit
 * programs this validates). A program that blocks inside a NuttX primitive
 * would raise a NuttX SVC with the latch set and be mis-decoded as Linux. The
 * robust discriminator is privilege-based (an unprivileged svc is Linux), which
 * needs the unprivileged-task model NuttX-Cortex-M cannot fully provide (see
 * ove_protected) — the same NOMMU ceiling already documented. Single-program,
 * non-blocking scope here; not yet a general process model.
 */

/* NuttX-internal SVCall handler — chained (not patched) for non-Linux svcs.
 * Declared in arch/arm/src/common/arm_internal.h, which is off the app include
 * path; restated here as the one internal-symbol coupling (see header note). */
extern int arm_svcall(int irq, void *context, void *arg);

/* ARMv7-M SVCall exception/IRQ number (== NuttX's internal NVIC_IRQ_SVCALL). */
#define OVE_LNX_IRQ_SVCALL 11

static ove_lnx_proc_t *g_proc; /* running program's context (NULL when idle) */
static jmp_buf g_jmp;	       /* recovery point inside ove_lnx_run() */
static volatile int g_active;  /* latch: a Linux program is executing */
static uintptr_t g_code_lo;    /* loaded program's address range [lo, hi): an */
static uintptr_t g_code_hi;    /* svc from within it is Linux, else it's NuttX's */

typedef void (*ove_lnx_entry_fn)(void);

/* Resumed in thread mode (via the rewritten exception-return PC) on program
 * exit; unwinds back to ove_lnx_run(). */
static void ove_lnx_recover(void)
{
	longjmp(g_jmp, 1);
}

static int ove_lnx_svc_handler(int irq, void *context, void *arg)
{
	uint32_t *regs = (uint32_t *)context;

	/* Latch clear, or a svc from outside the loaded program (one of NuttX's
	 * own svc #0 calls — context switch / scheduling — which share the same
	 * instruction): hand it to NuttX. The return PC discriminates: the program
	 * lives in its own (RAM) region, NuttX's code does not. */
	if (!g_active || !g_proc || !regs)
		return arm_svcall(irq, context, arg);
	uintptr_t pc = (uintptr_t)regs[REG_PC];
	if (pc < g_code_lo || pc >= g_code_hi)
		return arm_svcall(irq, context, arg);

	long nr = (long)(int32_t)regs[REG_R7];

	if (nr == OVE_LNX_NR_exit || nr == OVE_LNX_NR_exit_group) {
		(void)ove_lnx_syscall(g_proc, nr, (int32_t)regs[REG_R0], 0, 0, 0, 0, 0);
		regs[REG_PC] = (uint32_t)&ove_lnx_recover & ~1u;
		regs[REG_XPSR] |= (1u << 24); /* keep Thumb state on exception return */
		return 0;
	}

	long r = ove_lnx_syscall(g_proc, nr, (int32_t)regs[REG_R0], (int32_t)regs[REG_R1],
				 (int32_t)regs[REG_R2], (int32_t)regs[REG_R3],
				 (int32_t)regs[REG_R4], (int32_t)regs[REG_R5]);
	regs[REG_R0] = (uint32_t)r; /* syscall result back to the program in r0 */
	return 0;
}

int ove_lnx_run(ove_lnx_proc_t *proc, const ove_flat_t *prog)
{
	if (!proc || !prog || !prog->entry)
		return OVE_ERR_INVALID_PARAM;

	g_proc = proc;
	g_code_lo = prog->text_base;
	g_code_hi = prog->text_base + prog->region_used;
	ove_lnx_entry_fn entry = (ove_lnx_entry_fn)prog->entry;
	irq_attach(OVE_LNX_IRQ_SVCALL, ove_lnx_svc_handler, NULL);

	if (setjmp(g_jmp) == 0) {
		g_active = 1;
		entry();	  /* run the program; its svc #0 -> ove_lnx_svc_handler */
		proc->exited = 1; /* returned without exit_group: treat as clean */
	}
	/* else: resumed from ove_lnx_recover() after exit / exit_group. */
	g_active = 0;

	irq_attach(OVE_LNX_IRQ_SVCALL, arm_svcall, NULL); /* restore NuttX's handler */
	g_proc = NULL;
	return OVE_OK;
}

#endif /* CONFIG_OVE_LINUX */
