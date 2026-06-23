/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * NuttX seam for the Linux personality. The engine-agnostic run loop, svc
 * dispatch, and signal delivery live in backends/common/ove_lnx_run.c; this file
 * supplies only the NuttX-specific bits: the svc trap, the program memory, and
 * the task spawn (via the ove_lnx_engine vtable).
 *
 * PHASE 1 (functional parity): the program runs PRIVILEGED on the default
 * CONFIG_BUILD_FLAT build. Its `svc #0` shares the SVCall exception with NuttX's
 * OWN `svc #0` (its syscall/context-switch ABI), so we discriminate at runtime:
 * irq_attach() re-routes SVCall (exception 11) to our handler; a svc whose return
 * PC is inside the loaded program's region is a Linux syscall, and any other svc
 * (NuttX's scheduling svcs, from kernel .text) is chained to arm_svcall(). NuttX
 * builds unmodified (only the extern to arm_svcall + public scheduler APIs).
 *
 * Each program runs as a real NuttX task created with nxtask_init() given its own
 * region as the task stack, with the initial register context set to the uClinux
 * entry state (resume replays the captured vfork context). Phase 2 (unprivileged
 * + MPU) would need CONFIG_BUILD_PROTECTED.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX)

#include <nuttx/irq.h>	 /* irq_attach, xcpt_t; pulls arch/irq.h for REG_* */
#include <nuttx/sched.h> /* nxtask_init, nxtask_activate, struct task_tcb_s */
#include <sched.h>	 /* task_delete */
#include <stdint.h>
#include <string.h>
#include <unistd.h> /* usleep */

#include "../common/ove_lnx_run.h"

/* NuttX's own SVCall handler — chained (not patched) for non-Linux svcs.
 * Declared in arch/arm/src/common/arm_internal.h (off the app include path);
 * restated here as the one internal-symbol coupling. */
extern int arm_svcall(int irq, void *context, void *arg);

#define OVE_LNX_IRQ_SVCALL 11 /* == NuttX's internal NVIC_IRQ_SVCALL */
#define SLOT_PRIO 60	      /* below the run-loop/main task (100) */

/* ---- NuttX-specific state -------------------------------------------------- */
static uint8_t prog_regions[OVE_LNX_NREG][OVE_LNX_PROG_REGION_SIZE] __attribute__((aligned(32)));
static uintptr_t g_region_stack_lo[OVE_LNX_NREG];
static struct task_tcb_s g_tcb[OVE_LNX_NSLOT];
static int g_pid[OVE_LNX_NSLOT];
static uintptr_t g_code_lo, g_code_hi; /* the running program's region (svc PC discriminator) */

/* The single used slot (the sequentialised model runs one at a time). */
static int current_slot(void)
{
	for (int i = 0; i < OVE_LNX_NSLOT; i++)
		if (g_ove_lnx_used[i])
			return i;
	return -1;
}

/* The SVCall interposer. */
static int ove_lnx_svc_handler(int irq, void *context, void *arg)
{
	uint32_t *regs = (uint32_t *)context;
	if (!g_ove_lnx_active || !regs)
		return arm_svcall(irq, context, arg);
	uintptr_t pc = (uintptr_t)regs[REG_PC];
	if (pc < g_code_lo || pc >= g_code_hi)
		return arm_svcall(irq, context, arg);
	int sidx = current_slot();
	if (sidx < 0)
		return arm_svcall(irq, context, arg);

	/* arm_doirq() skips re-saving the interrupted context for an SVCall whose
	 * regs[REG_R0] == SYS_restore_context (== 1) — but a Linux syscall's r0 can
	 * legitimately be 1 (e.g. ioctl(fd=1, ...)), in which case arm_doirq would
	 * exception-return from a stale/NULL xcp.regs and crash. Re-assert the
	 * running task's saved-regs pointer so the return replays OUR frame. */
	nxsched_self()->xcp.regs = regs;

	/* Populate the uniform frame, dispatch, write the modified HW regs back. */
	struct ove_lnx_frame f;
	f.r[0] = regs[REG_R0];
	f.r[1] = regs[REG_R1];
	f.r[2] = regs[REG_R2];
	f.r[3] = regs[REG_R3];
	f.r[4] = regs[REG_R4];
	f.r[5] = regs[REG_R5];
	f.r[6] = regs[REG_R6];
	f.r[7] = regs[REG_R7];
	f.r[8] = regs[REG_R8];
	f.r[9] = regs[REG_R9];
	f.r[10] = regs[REG_R10];
	f.r[11] = regs[REG_R11];
	f.r[12] = regs[REG_R12];
	f.r[13] = regs[REG_SP]; /* NuttX saves the program's pre-svc SP directly */
	f.r[14] = regs[REG_R14];
	f.r[15] = regs[REG_PC];
	f.xpsr = regs[REG_XPSR];

	ove_lnx_dispatch(&f, &g_ove_lnx_proc[sidx]);

	regs[REG_R0] = f.r[0];
	regs[REG_R1] = f.r[1];
	regs[REG_R2] = f.r[2];
	regs[REG_R3] = f.r[3];
	regs[REG_R12] = f.r[12];
	regs[REG_R14] = f.r[14];
	regs[REG_PC] = f.r[15];
	regs[REG_XPSR] = f.xpsr;
	return 0;
}

/* ---- the vtable: NuttX task spawn ------------------------------------------ */
static uint8_t *nuttx_region(int ridx)
{
	return prog_regions[ridx];
}

/* nxtask_init needs a main_t entry; we override REG_PC, so it is never called. */
static int slot_noentry(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	for (;;) {
	}
	return 0;
}

/* Create slot `sidx` as a NuttX task whose real stack is [stack_lo, sp_top). */
static int spawn_task(int sidx, uintptr_t stack_lo, uintptr_t sp_top)
{
	memset(&g_tcb[sidx], 0, sizeof(g_tcb[sidx]));
	g_tcb[sidx].cmn.flags = TCB_FLAG_TTYPE_TASK; /* static TCB: no FREE_TCB/FREE_STACK */
	if (nxtask_init(&g_tcb[sidx], "lnx", SLOT_PRIO, (void *)stack_lo,
			(uint32_t)(sp_top - stack_lo), slot_noentry, NULL, NULL, NULL) < 0)
		return -1;
	g_pid[sidx] = g_tcb[sidx].cmn.pid;
	g_ove_lnx_used[sidx] = 1;
	return 0;
}

static int nuttx_spawn_launch(int sidx, int ridx, const ove_flat_t *prog, void *entry, void *sp,
			      void *stack_lo)
{
	(void)prog;
	uint8_t *region = prog_regions[ridx];
	g_code_lo = (uintptr_t)region;
	g_code_hi = (uintptr_t)region + OVE_LNX_PROG_REGION_SIZE;
	g_region_stack_lo[ridx] = (uintptr_t)stack_lo;
	if (spawn_task(sidx, (uintptr_t)stack_lo, (uintptr_t)sp) != 0)
		return -1;
	uint32_t *regs = g_tcb[sidx].cmn.xcp.regs;
	regs[REG_PC] = (uint32_t)(uintptr_t)entry & ~1u;
	regs[REG_SP] = (uint32_t)(uintptr_t)sp;
	regs[REG_R0] = 0; /* static fini = NULL (uClinux entry convention) */
	nxtask_activate(&g_tcb[sidx].cmn);
	return 0;
}

static void nuttx_spawn_resume(int sidx, int ridx, long r0val)
{
	uint8_t *region = prog_regions[ridx];
	g_code_lo = (uintptr_t)region;
	g_code_hi = (uintptr_t)region + OVE_LNX_PROG_REGION_SIZE;
	if (spawn_task(sidx, g_region_stack_lo[ridx], (uintptr_t)g_ove_lnx_vfork.sp) != 0)
		return;
	uint32_t *regs = g_tcb[sidx].cmn.xcp.regs;
	regs[REG_R4] = g_ove_lnx_vfork.r4_11[0];
	regs[REG_R5] = g_ove_lnx_vfork.r4_11[1];
	regs[REG_R6] = g_ove_lnx_vfork.r4_11[2];
	regs[REG_R7] = g_ove_lnx_vfork.r4_11[3];
	regs[REG_R8] = g_ove_lnx_vfork.r4_11[4];
	regs[REG_R9] = g_ove_lnx_vfork.r4_11[5];
	regs[REG_R10] = g_ove_lnx_vfork.r4_11[6];
	regs[REG_R11] = g_ove_lnx_vfork.r4_11[7];
	regs[REG_R12] = g_ove_lnx_vfork.r12;
	regs[REG_R14] = g_ove_lnx_vfork.lr;
	regs[REG_SP] = g_ove_lnx_vfork.sp;
	regs[REG_PC] = g_ove_lnx_vfork.pc & ~1u;
	regs[REG_R0] = (uint32_t)r0val;
	nxtask_activate(&g_tcb[sidx].cmn);
}

static void nuttx_abort_slot(int sidx)
{
	if (g_ove_lnx_used[sidx] && g_pid[sidx] >= 0)
		(void)task_delete(g_pid[sidx]);
	g_ove_lnx_used[sidx] = 0;
	g_pid[sidx] = -1;
}

static void nuttx_sleep_ms(unsigned ms)
{
	usleep(ms * 1000u);
}

static const struct ove_lnx_engine g_nuttx_engine = {
	.region = nuttx_region,
	.spawn_launch = nuttx_spawn_launch,
	.spawn_resume = nuttx_spawn_resume,
	.abort_slot = nuttx_abort_slot,
	.sleep_ms = nuttx_sleep_ms,
};

int ove_lnx_run(const ove_lnx_run_config_t *cfg, const char *path, int argc,
		const char *const argv[])
{
	for (int i = 0; i < OVE_LNX_NSLOT; i++)
		g_pid[i] = -1;
	irq_attach(OVE_LNX_IRQ_SVCALL, ove_lnx_svc_handler, NULL);
	int rc = ove_lnx_run_common(&g_nuttx_engine, cfg, path, argc, argv);
	irq_attach(OVE_LNX_IRQ_SVCALL, arm_svcall, NULL); /* restore NuttX's handler */
	return rc;
}

#endif /* CONFIG_OVE_LINUX */
