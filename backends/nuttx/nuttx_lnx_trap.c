/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * NuttX engine seam for the Linux personality (see include/ove/linux/run.h).
 * The NuttX analogue of backends/{zephyr/zephyr_lnx.c, freertos/freertos_lnx.c}:
 * it traps a loaded program's `svc #0`, runs the NOMMU process model
 * (sequentialised vfork/exec/wait + signal delivery + the run loop) on NuttX
 * tasks, and dispatches into the engine-agnostic syscall core (ove_lnx_syscall).
 *
 * PHASE 1 (functional parity): the program runs PRIVILEGED on the default
 * CONFIG_BUILD_FLAT build. Its `svc #0` shares the SVCall exception with NuttX's
 * OWN `svc #0` (its syscall/context-switch ABI), so we discriminate at runtime:
 * the public irq_attach() re-routes SVCall (exception 11) to ove_lnx_svc_handler;
 * a svc whose return PC is inside the loaded program's RAM region is a Linux
 * syscall (dispatch it), and any other svc — NuttX's own scheduling svcs, which
 * come from kernel .text — is chained to NuttX's arm_svcall(). NuttX builds
 * unmodified (only the extern to arm_svcall + public/internal scheduler APIs).
 *
 * Each program runs as a real NuttX task created with nxtask_init(), given the
 * program's own region as its REAL stack (so NuttX tracks the SP the program
 * actually runs on — no trampoline, no `mov sp` stack abandonment). We then set
 * the task's INITIAL register context directly to the uClinux entry state
 * (sp -> argc block, r0 = 0, pc -> program entry), bypassing nxtask_start. This
 * is the same way NuttX's own loaders hand a loaded binary its stack + entry.
 * Phase 2 (unprivileged + MPU) would need CONFIG_BUILD_PROTECTED.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX)

#include <nuttx/irq.h>	 /* irq_attach, xcpt_t; pulls arch/irq.h for REG_* */
#include <nuttx/sched.h> /* nxtask_init, nxtask_activate, struct task_tcb_s */
#include <sched.h>	 /* task_delete */
#include <stdint.h>
#include <string.h>
#include <unistd.h> /* usleep */

#include "ove/arena.h"
#include "ove/loader.h"
#include "ove/linux/run.h"
#include "ove/linux/syscall.h"

/* NuttX's own SVCall handler — chained (not patched) for non-Linux svcs.
 * Declared in arch/arm/src/common/arm_internal.h (off the app include path);
 * restated here as the one internal-symbol coupling. */
extern int arm_svcall(int irq, void *context, void *arg);

/* ARMv7-M SVCall exception/IRQ number (== NuttX's internal NVIC_IRQ_SVCALL). */
#define OVE_LNX_IRQ_SVCALL 11

/* ---- program memory: two regions, so a parent + child image coexist -------- */
#define PROG_REGION_SIZE 0x60000u /* 384K: BusyBox loads ~129K + arena + stack */
#define PROG_ARENA_SIZE 0x18000u  /* 96K heap for the program */
#define NREG 2
#define NSLOT 2
#define SLOT_PRIO 60 /* NuttX priority — below the run-loop/main task (100), so */
		     /* the run loop preempts a parked program                  */

static uint8_t prog_regions[NREG][PROG_REGION_SIZE] __attribute__((aligned(32)));
static ove_arena_t g_arenas[NREG];
static uintptr_t g_region_stack_lo[NREG]; /* the program-stack base per region */

/* ---- per-process slots (NuttX tasks via nxtask_init) ----------------------- */
/* The sequentialised model keeps exactly ONE slot `used` at a time (the other
 * is deleted), so the running program is always the single used slot. The TCB
 * is static (no TCB_FLAG_FREE_TCB) and the stack is the program's region (no
 * TCB_FLAG_FREE_STACK), so task_delete frees neither — only the kernel group. */
struct slot {
	ove_lnx_proc_t proc;
	struct task_tcb_s tcb;
	int pid;
	int used;
};
static struct slot g_slots[NSLOT];

static const ove_lnx_run_config_t *g_cfg;
static volatile int g_lnx_active;
static uintptr_t g_code_lo; /* the running program's region: a svc whose PC is */
static uintptr_t g_code_hi; /* in [lo,hi) is Linux, else it's one of NuttX's   */

/* Captured parent context at a vfork svc, replayed to resume parent + child. */
struct resume_ctx {
	uint32_t r4_11[8];
	uint32_t r12;
	uint32_t lr;
	uint32_t sp;
	uint32_t pc;
};
static struct resume_ctx g_vfork_ctx;
static volatile int g_vfork_pending;

/* Saved interrupted context for an in-flight signal handler (r4-r11 preserved by
 * the C handler, so not saved). No nesting. */
static struct {
	uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
	int active;
} g_sig_save;

static volatile int g_tty_isig = 1;
static volatile int g_pending_sig;

int ove_lnx_tty_isig(void)
{
	return g_tty_isig;
}

void ove_lnx_post_signal(int sig)
{
	g_pending_sig = sig;
}

static void park_loop(void)
{
	for (;;) {
	}
}

static struct slot *current_slot(void)
{
	for (int i = 0; i < NSLOT; i++)
		if (g_slots[i].used)
			return &g_slots[i];
	return NULL;
}

/* Deliver signal `sig` to slot `s`; `ret` is the interrupted syscall's result. */
static void deliver_signal(uint32_t *regs, struct slot *s, int sig, long ret)
{
	if (sig < 1 || sig >= OVE_LNX_NSIG) {
		regs[REG_R0] = (uint32_t)-OVE_LNX_EINVAL;
		return;
	}
	uintptr_t h = s->proc.sig_handler[sig];
	if (h == OVE_LNX_SIG_IGN) {
		regs[REG_R0] = (uint32_t)ret;
		return;
	}
	if (h == OVE_LNX_SIG_DFL) {
		s->proc.exited = 1;
		s->proc.exit_status = 128 + sig;
		regs[REG_PC] = ((uint32_t)&park_loop) & ~1u;
		regs[REG_XPSR] |= (1u << 24); /* keep Thumb state */
		return;
	}
	g_sig_save.r0 = (uint32_t)ret;
	g_sig_save.r1 = regs[REG_R1];
	g_sig_save.r2 = regs[REG_R2];
	g_sig_save.r3 = regs[REG_R3];
	g_sig_save.r12 = regs[REG_R12];
	g_sig_save.lr = regs[REG_R14];
	g_sig_save.pc = regs[REG_PC];
	g_sig_save.xpsr = regs[REG_XPSR];
	g_sig_save.active = 1;
	regs[REG_PC] = h & ~1u;				/* pc -> handler (Thumb via xPSR.T) */
	regs[REG_R0] = (uint32_t)sig;			/* r0 = signo */
	regs[REG_R14] = s->proc.sig_restorer[sig] | 1u; /* lr -> sa_restorer */
	regs[REG_XPSR] |= (1u << 24);
}

static void sig_restore(uint32_t *regs)
{
	if (!g_sig_save.active)
		return;
	regs[REG_R0] = g_sig_save.r0;
	regs[REG_R1] = g_sig_save.r1;
	regs[REG_R2] = g_sig_save.r2;
	regs[REG_R3] = g_sig_save.r3;
	regs[REG_R12] = g_sig_save.r12;
	regs[REG_R14] = g_sig_save.lr;
	regs[REG_PC] = g_sig_save.pc & ~1u;
	regs[REG_XPSR] = g_sig_save.xpsr;
	g_sig_save.active = 0;
}

/* The SVCall interposer (pure C — NuttX hands us the full saved frame). */
static int ove_lnx_svc_handler(int irq, void *context, void *arg)
{
	uint32_t *regs = (uint32_t *)context;

	/* Not a Linux svc (latch clear, or a NuttX svc — its PC is in kernel .text,
	 * never inside the loaded program's region): chain to NuttX. */
	if (!g_lnx_active || !regs)
		return arm_svcall(irq, context, arg);
	uintptr_t pc = (uintptr_t)regs[REG_PC];
	if (pc < g_code_lo || pc >= g_code_hi)
		return arm_svcall(irq, context, arg);
	struct slot *s = current_slot();
	if (!s)
		return arm_svcall(irq, context, arg);

	/* arm_doirq() skips re-saving the interrupted context for an SVCall whose
	 * regs[REG_R0] == SYS_restore_context (== 1) — but a Linux syscall's r0 can
	 * legitimately be 1 (e.g. ioctl(fd=1, ...)), in which case arm_doirq would
	 * exception-return from a stale/NULL xcp.regs and crash. Re-assert the
	 * running task's saved-regs pointer so the return replays OUR frame. */
	nxsched_self()->xcp.regs = regs;

	long nr = (long)(int32_t)regs[REG_R7];

	/* Track the tty ISIG mode so a console ^C knows whether to raise SIGINT. */
	if (nr == OVE_LNX_NR_ioctl) {
		unsigned long cmd = regs[REG_R1];
		if (cmd == OVE_LNX_TCSETS || cmd == OVE_LNX_TCSETSW || cmd == OVE_LNX_TCSETSF) {
			const ove_lnx_termios *t = (const ove_lnx_termios *)(uintptr_t)regs[REG_R2];
			if (t)
				g_tty_isig = (t->c_lflag & OVE_LNX_ISIG) ? 1 : 0;
		}
	}
	if (nr == OVE_LNX_NR_kill || nr == OVE_LNX_NR_tkill || nr == OVE_LNX_NR_tgkill) {
		int sig = (nr == OVE_LNX_NR_tgkill) ? (int)regs[REG_R2] : (int)regs[REG_R1];
		deliver_signal(regs, s, sig, 0);
		return 0;
	}
	if (nr == OVE_LNX_NR_rt_sigreturn || nr == OVE_LNX_NR_sigreturn) {
		sig_restore(regs);
		return 0;
	}
	if (nr == OVE_LNX_NR_vfork || nr == OVE_LNX_NR_fork) {
		g_vfork_ctx.r4_11[0] = regs[REG_R4];
		g_vfork_ctx.r4_11[1] = regs[REG_R5];
		g_vfork_ctx.r4_11[2] = regs[REG_R6];
		g_vfork_ctx.r4_11[3] = regs[REG_R7];
		g_vfork_ctx.r4_11[4] = regs[REG_R8];
		g_vfork_ctx.r4_11[5] = regs[REG_R9];
		g_vfork_ctx.r4_11[6] = regs[REG_R10];
		g_vfork_ctx.r4_11[7] = regs[REG_R11];
		g_vfork_ctx.r12 = regs[REG_R12];
		g_vfork_ctx.lr = regs[REG_R14];
		g_vfork_ctx.sp = regs[REG_SP];
		g_vfork_ctx.pc = regs[REG_PC] | 1u;
		g_vfork_pending = 1;
		regs[REG_PC] = ((uint32_t)&park_loop) & ~1u;
		regs[REG_XPSR] |= (1u << 24);
		return 0;
	}

	long r = ove_lnx_syscall(&s->proc, nr, (int32_t)regs[REG_R0], (int32_t)regs[REG_R1],
				 (int32_t)regs[REG_R2], (int32_t)regs[REG_R3],
				 (int32_t)regs[REG_R4], (int32_t)regs[REG_R5]);
	if (r == -OVE_LNX_ENOSYS && g_cfg && g_cfg->on_enosys)
		g_cfg->on_enosys(nr);
	if (s->proc.exited || s->proc.exec_pending) {
		regs[REG_PC] = ((uint32_t)&park_loop) & ~1u;
		regs[REG_XPSR] |= (1u << 24);
		return 0;
	}
	if (g_pending_sig) {
		int sig = g_pending_sig;
		g_pending_sig = 0;
		deliver_signal(regs, s, sig, r);
		return 0;
	}
	regs[REG_R0] = (uint32_t)r;
	return 0;
}

/* ---- slot lifecycle (NuttX tasks) ------------------------------------------ */
/* nxtask_init requires a main_t entry; we override REG_PC to the program entry,
 * so this is never actually called. */
static int slot_noentry(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	for (;;) {
	}
	return 0;
}

/* Create slot `sidx` as a NuttX task whose real stack is [stack_lo, sp_top), set
 * its initial register context, and activate it. */
static int spawn_task(int sidx, uintptr_t stack_lo, uintptr_t sp_top)
{
	struct slot *s = &g_slots[sidx];
	memset(&s->tcb, 0, sizeof(s->tcb));
	s->tcb.cmn.flags = TCB_FLAG_TTYPE_TASK; /* no FREE_TCB (static TCB) */
	if (nxtask_init(&s->tcb, "lnx", SLOT_PRIO, (void *)stack_lo, (uint32_t)(sp_top - stack_lo),
			slot_noentry, NULL, NULL, NULL) < 0)
		return -1;
	s->pid = s->tcb.cmn.pid;
	s->used = 1;
	return 0;
}

static int launch_slot(int sidx, int ridx, const uint8_t *data, size_t len, int pid, int ppid,
		       int argc, const char *const argv[])
{
	uint8_t *region = prog_regions[ridx];
	ove_flat_t prog;
	if (ove_loader_load_flat(&prog, data, len, region, PROG_REGION_SIZE) != OVE_OK)
		return -1;
	uint8_t *rw = region + ((prog.region_used + 15u) & ~15u);
	uint8_t *rw_end = region + PROG_REGION_SIZE;
	ove_arena_init(&g_arenas[ridx], rw, PROG_ARENA_SIZE);
	ove_lnx_proc_init(&g_slots[sidx].proc, &g_arenas[ridx], 0x8000);
	g_slots[sidx].proc.write_fn = g_cfg->write_fn;
	g_slots[sidx].proc.read_fn = g_cfg->read_fn;
	g_slots[sidx].proc.io_ctx = g_cfg->io_ctx;
	g_slots[sidx].proc.pid = pid;
	g_slots[sidx].proc.ppid = ppid;
	ove_lnx_proc_set_rootfs(&g_slots[sidx].proc, g_cfg->rootfs, g_cfg->rootfs_count);

	uint8_t *stack_lo = rw + PROG_ARENA_SIZE;
	/* The argc block sits at the top of [stack_lo, rw_end); the program's usable
	 * stack is [stack_lo, sp) — that is what NuttX tracks for this task. */
	void *sp = ove_lnx_setup_stack(stack_lo, (size_t)(rw_end - stack_lo), argc, argv, NULL);
	if (!sp)
		return -1;
	g_region_stack_lo[ridx] = (uintptr_t)stack_lo;
	g_code_lo = (uintptr_t)region;
	g_code_hi = (uintptr_t)region + PROG_REGION_SIZE;

	if (spawn_task(sidx, (uintptr_t)stack_lo, (uintptr_t)sp) != 0)
		return -1;
	uint32_t *regs = g_slots[sidx].tcb.cmn.xcp.regs;
	regs[REG_PC] = (uint32_t)prog.entry & ~1u;
	regs[REG_SP] = (uint32_t)(uintptr_t)sp;
	regs[REG_R0] = 0; /* static fini = NULL (uClinux entry convention) */
	nxtask_activate(&g_slots[sidx].tcb.cmn);
	return 0;
}

static void resume_slot(int sidx, int ridx, long r0val)
{
	uint8_t *region = prog_regions[ridx];
	g_code_lo = (uintptr_t)region;
	g_code_hi = (uintptr_t)region + PROG_REGION_SIZE;
	if (spawn_task(sidx, g_region_stack_lo[ridx], (uintptr_t)g_vfork_ctx.sp) != 0)
		return;
	uint32_t *regs = g_slots[sidx].tcb.cmn.xcp.regs;
	regs[REG_R4] = g_vfork_ctx.r4_11[0];
	regs[REG_R5] = g_vfork_ctx.r4_11[1];
	regs[REG_R6] = g_vfork_ctx.r4_11[2];
	regs[REG_R7] = g_vfork_ctx.r4_11[3];
	regs[REG_R8] = g_vfork_ctx.r4_11[4];
	regs[REG_R9] = g_vfork_ctx.r4_11[5];
	regs[REG_R10] = g_vfork_ctx.r4_11[6];
	regs[REG_R11] = g_vfork_ctx.r4_11[7];
	regs[REG_R12] = g_vfork_ctx.r12;
	regs[REG_R14] = g_vfork_ctx.lr;
	regs[REG_SP] = g_vfork_ctx.sp;
	regs[REG_PC] = g_vfork_ctx.pc & ~1u;
	regs[REG_R0] = (uint32_t)r0val;
	nxtask_activate(&g_slots[sidx].tcb.cmn);
}

static void abort_slot(int sidx)
{
	if (g_slots[sidx].used && g_slots[sidx].pid >= 0)
		(void)task_delete(g_slots[sidx].pid);
	g_slots[sidx].used = 0;
	g_slots[sidx].pid = -1;
}

int ove_lnx_run(const ove_lnx_run_config_t *cfg, const char *path, int argc,
		const char *const argv[])
{
	if (!cfg || !cfg->rootfs || !path || argc < 1 || !argv)
		return OVE_LNX_RUN_ELAUNCH;
	g_cfg = cfg;
	for (int i = 0; i < NSLOT; i++) {
		g_slots[i].used = 0;
		g_slots[i].pid = -1;
	}
	g_vfork_pending = 0;
	g_pending_sig = 0;
	g_tty_isig = 1;
	g_sig_save.active = 0;

	int bb = -1;
	for (int i = 0; i < cfg->rootfs_count; i++)
		if (strcmp(cfg->rootfs[i].path, path) == 0) {
			bb = i;
			break;
		}
	if (bb < 0 || !cfg->rootfs[bb].data)
		return OVE_LNX_RUN_ELAUNCH;

	/* Interpose the SVCall handler for the duration of the run. */
	irq_attach(OVE_LNX_IRQ_SVCALL, ove_lnx_svc_handler, NULL);
	g_lnx_active = 1;

	int rc = OVE_LNX_RUN_ETIMEOUT;
	if (launch_slot(0, 0, cfg->rootfs[bb].data, cfg->rootfs[bb].size, 1, 0, argc, argv) != 0) {
		rc = OVE_LNX_RUN_ELAUNCH;
		goto done;
	}

	int next_pid = 2;
	int cur_child = 2;
	for (int i = 0; i < 8000; i++) {
		if (g_vfork_pending) {
			g_vfork_pending = 0;
			cur_child = next_pid++;
			g_slots[1].proc = g_slots[0].proc;
			g_slots[1].proc.pid = cur_child;
			g_slots[1].proc.ppid = 1;
			g_slots[1].proc.exited = 0;
			g_slots[1].proc.exec_pending = 0;
			g_slots[1].proc.child_count = 0;
			abort_slot(0);
			resume_slot(1, 0, 0);
			continue;
		}
		if (g_slots[1].used && g_slots[1].proc.exec_pending) {
			int idx = g_slots[1].proc.exec_file_idx;
			int eargc = g_slots[1].proc.exec_argc;
			static char args[OVE_LNX_EXEC_ARGBUF];
			static const char *ptrs[OVE_LNX_EXEC_MAXARGS + 1];
			size_t off = 0;
			for (int j = 0; j < eargc; j++) {
				size_t n = strlen(g_slots[1].proc.exec_argv[j]) + 1;
				memcpy(args + off, g_slots[1].proc.exec_argv[j], n);
				ptrs[j] = args + off;
				off += n;
			}
			ptrs[eargc] = NULL;
			ove_lnx_fd_t saved_fds[OVE_LNX_MAX_FDS];
			memcpy(saved_fds, g_slots[1].proc.fds, sizeof(saved_fds));
			abort_slot(1);
			if (launch_slot(1, 1, cfg->rootfs[idx].data, cfg->rootfs[idx].size,
					cur_child, 1, eargc, ptrs) != 0) {
				rc = OVE_LNX_RUN_EEXEC;
				break;
			}
			memcpy(g_slots[1].proc.fds, saved_fds, sizeof(saved_fds));
			continue;
		}
		if (g_slots[1].used && g_slots[1].proc.exited) {
			int status = g_slots[1].proc.exit_status;
			abort_slot(1);
			ove_lnx_proc_t *par = &g_slots[0].proc;
			if (par->child_count < OVE_LNX_MAX_CHILD) {
				par->child_pid[par->child_count] = cur_child;
				par->child_status[par->child_count] = status;
				par->child_count++;
			}
			resume_slot(0, 0, cur_child);
			continue;
		}
		if (g_slots[0].used && g_slots[0].proc.exited) {
			rc = g_slots[0].proc.exit_status;
			break;
		}
		usleep(1000);
	}

done:
	for (int i = 0; i < NSLOT; i++)
		abort_slot(i);
	g_lnx_active = 0;
	irq_attach(OVE_LNX_IRQ_SVCALL, arm_svcall, NULL); /* restore NuttX's handler */
	return rc;
}

#endif /* CONFIG_OVE_LINUX */
