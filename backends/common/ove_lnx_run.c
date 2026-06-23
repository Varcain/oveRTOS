/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Engine-agnostic Linux-personality run loop + svc dispatch + signal delivery,
 * shared by the Zephyr / FreeRTOS / NuttX seams (see ove_lnx_run.h). The
 * NOMMU process model lives here once; each seam supplies the svc trap, the
 * program memory, and the task spawn through a small vtable.
 *
 * Sequentialised vfork/exec/wait (observationally identical to vfork for the
 * shell pattern, since the parent waitpid()s anyway):
 *  - vfork: capture the parent's full resume context (r4-r11/r12/lr/sp/pc) and
 *    park it; the run loop spawns a CHILD resuming at that context with r0=0,
 *    sharing the parent's region until it execs.
 *  - execve: the run loop loads the new image into a SECOND region.
 *  - child exit: queue the status on the parent for wait4, then resume the
 *    parent at the captured context with r0 = child_pid.
 */

#include <string.h>

#include "ove/arena.h"
#include "ove_lnx_run.h"

/* ---- shared state ---------------------------------------------------------- */
struct ove_lnx_resume_ctx g_ove_lnx_vfork;
ove_lnx_proc_t g_ove_lnx_proc[OVE_LNX_NSLOT];
int g_ove_lnx_used[OVE_LNX_NSLOT];
volatile int g_ove_lnx_active;

static ove_arena_t g_arenas[OVE_LNX_NREG];
static const ove_lnx_run_config_t *g_cfg;
static volatile int g_vfork_pending;

/* Saved interrupted context for an in-flight signal handler. r4-r11 are NOT
 * saved (a C handler preserves them, so they are already correct at sigreturn).
 * No nesting (one handler at a time). */
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

void ove_lnx_park_loop(void)
{
	for (;;) {
	}
}

/* ---- signal delivery (over the uniform frame) ------------------------------ */
/* Deliver signal `sig` to `proc`; `ret` is the interrupted syscall's result
 * (0 for a kill/tkill, -EINTR for a console-interrupted read). */
static void deliver_signal(struct ove_lnx_frame *f, ove_lnx_proc_t *proc, int sig, long ret)
{
	if (sig < 1 || sig >= OVE_LNX_NSIG) {
		f->r[0] = (uint32_t)-OVE_LNX_EINVAL;
		return;
	}
	uintptr_t h = proc->sig_handler[sig];
	if (h == OVE_LNX_SIG_IGN) {
		f->r[0] = (uint32_t)ret;
		return;
	}
	if (h == OVE_LNX_SIG_DFL) {
		proc->exited = 1;
		proc->exit_status = 128 + sig;
		f->r[15] = ((uint32_t)&ove_lnx_park_loop) & ~1u;
		f->xpsr |= (1u << 24); /* keep Thumb state */
		return;
	}
	g_sig_save.r0 = (uint32_t)ret;
	g_sig_save.r1 = f->r[1];
	g_sig_save.r2 = f->r[2];
	g_sig_save.r3 = f->r[3];
	g_sig_save.r12 = f->r[12];
	g_sig_save.lr = f->r[14];
	g_sig_save.pc = f->r[15];
	g_sig_save.xpsr = f->xpsr;
	g_sig_save.active = 1;
	f->r[15] = h & ~1u;			 /* pc -> handler (Thumb via xPSR.T) */
	f->r[0] = (uint32_t)sig;		 /* r0 = signo */
	f->r[14] = proc->sig_restorer[sig] | 1u; /* lr -> sa_restorer */
	f->xpsr |= (1u << 24);
}

/* rt_sigreturn: restore the context saved at delivery. */
static void sig_restore(struct ove_lnx_frame *f)
{
	if (!g_sig_save.active)
		return;
	f->r[0] = g_sig_save.r0;
	f->r[1] = g_sig_save.r1;
	f->r[2] = g_sig_save.r2;
	f->r[3] = g_sig_save.r3;
	f->r[12] = g_sig_save.r12;
	f->r[14] = g_sig_save.lr;
	f->r[15] = g_sig_save.pc & ~1u;
	f->xpsr = g_sig_save.xpsr;
	g_sig_save.active = 0;
}

/* ---- the syscall dispatch body --------------------------------------------- */
void ove_lnx_dispatch(struct ove_lnx_frame *f, ove_lnx_proc_t *proc)
{
	long nr = (long)(int32_t)f->r[7];

	/* Track the tty ISIG mode so a console ^C knows whether to raise SIGINT
	 * (canonical) or pass ^C through (the shell's raw line editor). */
	if (nr == OVE_LNX_NR_ioctl) {
		unsigned long cmd = f->r[1];
		if (cmd == OVE_LNX_TCSETS || cmd == OVE_LNX_TCSETSW || cmd == OVE_LNX_TCSETSF) {
			const ove_lnx_termios *t = (const ove_lnx_termios *)(uintptr_t)f->r[2];
			if (t)
				g_tty_isig = (t->c_lflag & OVE_LNX_ISIG) ? 1 : 0;
		}
	}
	if (nr == OVE_LNX_NR_kill || nr == OVE_LNX_NR_tkill || nr == OVE_LNX_NR_tgkill) {
		int sig = (nr == OVE_LNX_NR_tgkill) ? (int)f->r[2] : (int)f->r[1];
		deliver_signal(f, proc, sig, 0);
		return;
	}
	if (nr == OVE_LNX_NR_rt_sigreturn || nr == OVE_LNX_NR_sigreturn) {
		sig_restore(f);
		return;
	}
	if (nr == OVE_LNX_NR_vfork || nr == OVE_LNX_NR_fork) {
		for (int i = 0; i < 8; i++)
			g_ove_lnx_vfork.r4_11[i] = f->r[4 + i];
		g_ove_lnx_vfork.r12 = f->r[12];
		g_ove_lnx_vfork.lr = f->r[14];
		g_ove_lnx_vfork.sp = f->r[13]; /* the seam set r[13] = the pre-svc SP */
		g_ove_lnx_vfork.pc = f->r[15] | 1u;
		g_vfork_pending = 1;
		f->r[15] = ((uint32_t)&ove_lnx_park_loop) & ~1u;
		f->xpsr |= (1u << 24);
		return;
	}

	long r = ove_lnx_syscall(proc, nr, (int32_t)f->r[0], (int32_t)f->r[1], (int32_t)f->r[2],
				 (int32_t)f->r[3], (int32_t)f->r[4], (int32_t)f->r[5]);
	if (r == -OVE_LNX_ENOSYS && g_cfg && g_cfg->on_enosys)
		g_cfg->on_enosys(nr);
	if (proc->exited || proc->exec_pending) {
		f->r[15] = ((uint32_t)&ove_lnx_park_loop) & ~1u;
		f->xpsr |= (1u << 24);
		return;
	}
	/* A console ^C latched a signal during this syscall (e.g. a read): deliver
	 * it now, resuming the syscall with its result (-EINTR) — the Linux
	 * at-the-boundary async-delivery model. */
	if (g_pending_sig) {
		int sig = g_pending_sig;
		g_pending_sig = 0;
		deliver_signal(f, proc, sig, r);
		return;
	}
	f->r[0] = (uint32_t)r;
}

/* ---- the run loop ---------------------------------------------------------- */
/* Load a bFLT into region ridx + set up slot sidx's proc, then spawn it. */
static int launch(const struct ove_lnx_engine *eng, int sidx, int ridx, const uint8_t *data,
		  size_t len, int pid, int ppid, int argc, const char *const argv[])
{
	uint8_t *region = eng->region(ridx);
	ove_flat_t prog;
	if (ove_loader_load_flat(&prog, data, len, region, OVE_LNX_PROG_REGION_SIZE) != OVE_OK)
		return -1;
	uint8_t *rw = region + ((prog.region_used + 15u) & ~15u);
	uint8_t *rw_end = region + OVE_LNX_PROG_REGION_SIZE;
	ove_arena_init(&g_arenas[ridx], rw, OVE_LNX_PROG_ARENA_SIZE);
	ove_lnx_proc_init(&g_ove_lnx_proc[sidx], &g_arenas[ridx], 0x8000);
	g_ove_lnx_proc[sidx].write_fn = g_cfg->write_fn;
	g_ove_lnx_proc[sidx].read_fn = g_cfg->read_fn;
	g_ove_lnx_proc[sidx].io_ctx = g_cfg->io_ctx;
	g_ove_lnx_proc[sidx].pid = pid;
	g_ove_lnx_proc[sidx].ppid = ppid;
	ove_lnx_proc_set_rootfs(&g_ove_lnx_proc[sidx], g_cfg->rootfs, g_cfg->rootfs_count);
	uint8_t *stack_lo = rw + OVE_LNX_PROG_ARENA_SIZE;
	void *sp = ove_lnx_setup_stack(stack_lo, (size_t)(rw_end - stack_lo), argc, argv, NULL);
	if (!sp)
		return -1;
	return eng->spawn_launch(sidx, ridx, &prog, (void *)prog.entry, sp, stack_lo);
}

int ove_lnx_run_common(const struct ove_lnx_engine *eng, const ove_lnx_run_config_t *cfg,
		       const char *path, int argc, const char *const argv[])
{
	if (!eng || !cfg || !cfg->rootfs || !path || argc < 1 || !argv)
		return OVE_LNX_RUN_ELAUNCH;
	g_cfg = cfg;
	for (int i = 0; i < OVE_LNX_NSLOT; i++)
		g_ove_lnx_used[i] = 0;
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

	g_ove_lnx_active = 1;
	if (launch(eng, 0, 0, cfg->rootfs[bb].data, cfg->rootfs[bb].size, 1, 0, argc, argv) != 0) {
		g_ove_lnx_active = 0;
		return OVE_LNX_RUN_ELAUNCH;
	}

	int rc = OVE_LNX_RUN_ETIMEOUT;
	int next_pid = 2;
	int cur_child = 2;
	for (int i = 0; i < 8000; i++) {
		if (g_vfork_pending) {
			g_vfork_pending = 0;
			cur_child = next_pid++;
			g_ove_lnx_proc[1] = g_ove_lnx_proc[0]; /* same arena/fds/rootfs */
			g_ove_lnx_proc[1].pid = cur_child;
			g_ove_lnx_proc[1].ppid = 1;
			g_ove_lnx_proc[1].exited = 0;
			g_ove_lnx_proc[1].exec_pending = 0;
			g_ove_lnx_proc[1].child_count = 0;
			eng->abort_slot(0); /* parent parked; resumed after child exit */
			eng->spawn_resume(1, 0, 0);
			continue;
		}
		if (g_ove_lnx_used[1] && g_ove_lnx_proc[1].exec_pending) {
			int idx = g_ove_lnx_proc[1].exec_file_idx;
			int eargc = g_ove_lnx_proc[1].exec_argc;
			static char args[OVE_LNX_EXEC_ARGBUF];
			static const char *ptrs[OVE_LNX_EXEC_MAXARGS + 1];
			size_t off = 0;
			for (int j = 0; j < eargc; j++) {
				size_t n = strlen(g_ove_lnx_proc[1].exec_argv[j]) + 1;
				memcpy(args + off, g_ove_lnx_proc[1].exec_argv[j], n);
				ptrs[j] = args + off;
				off += n;
			}
			ptrs[eargc] = NULL;
			/* fds survive execve (no close-on-exec): preserve the table so a pipe
			 * wired by dup2 before exec reaches the new image. */
			ove_lnx_fd_t saved_fds[OVE_LNX_MAX_FDS];
			memcpy(saved_fds, g_ove_lnx_proc[1].fds, sizeof(saved_fds));
			eng->abort_slot(1);
			if (launch(eng, 1, 1, cfg->rootfs[idx].data, cfg->rootfs[idx].size,
				   cur_child, 1, eargc, ptrs) != 0) {
				rc = OVE_LNX_RUN_EEXEC;
				break;
			}
			memcpy(g_ove_lnx_proc[1].fds, saved_fds, sizeof(saved_fds));
			continue;
		}
		if (g_ove_lnx_used[1] && g_ove_lnx_proc[1].exited) {
			int status = g_ove_lnx_proc[1].exit_status;
			eng->abort_slot(1);
			ove_lnx_proc_t *par = &g_ove_lnx_proc[0];
			if (par->child_count < OVE_LNX_MAX_CHILD) {
				par->child_pid[par->child_count] = cur_child;
				par->child_status[par->child_count] = status;
				par->child_count++;
			}
			eng->spawn_resume(0, 0, cur_child);
			continue;
		}
		if (g_ove_lnx_used[0] && g_ove_lnx_proc[0].exited) {
			rc = g_ove_lnx_proc[0].exit_status;
			break;
		}
		eng->sleep_ms(1);
	}
	/* Tear down any still-running slot tasks so a subsequent ove_lnx_run() starts
	 * clean and no leaked task starves the next program. */
	for (int i = 0; i < OVE_LNX_NSLOT; i++)
		if (g_ove_lnx_used[i])
			eng->abort_slot(i);
	g_ove_lnx_active = 0;
	return rc;
}
