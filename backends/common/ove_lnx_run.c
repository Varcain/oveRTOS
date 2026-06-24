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
#include "ove/thread.h"
#include "ove/time.h"
#include "ove_lnx_run.h"
#include "ove/linux/stats.h"

/* Parse the slot index from a Linux-program thread name "lnx<slot>". */
static int lnx_slot_of_name(const char *name)
{
	if (name[0] != 'l' || name[1] != 'n' || name[2] != 'x' || name[3] < '0' || name[3] > '9')
		return -1;
	int v = 0;
	for (const char *p = name + 3; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (*p - '0');
	return v;
}

/* Rebuild the ps/top snapshot from the live process SET + the RTOS kernel threads.
 * Run-loop thread only (ove_thread_list locks the scheduler — unsafe from the svc
 * handler). Each Linux slot's thread is named "lnx<slot>" so its CPU attributes to
 * the right process even with several running at once; the idle thread is folded
 * into /proc/stat idle, not shown as a process (else it crushes top's %CPU math). */
static void refresh_stats(void)
{
	struct ove_thread_info ti[OVE_LNX_MAX_KTHREAD];
	size_t n = 0;
	ove_thread_list(ti, OVE_LNX_MAX_KTHREAD, &n);

	/* 1. Charge each live Linux thread's CPU to its proc (slot from the name). */
	uint64_t idle = 0, busy = 0;
	for (size_t i = 0; i < n; i++) {
		const char *name = ti[i].name ? ti[i].name : "?";
		uint64_t rus = ti[i].state_times.running_us;
		int cls = ove_lnx_stats_classify(name);
		if (cls == 1) {
			idle += rus;
			continue;
		}
		busy += rus;
		if (cls == 2) {
			int s = lnx_slot_of_name(name);
			if (s >= 0 && s < OVE_LNX_NSLOT && g_ove_lnx_proc[s].alive)
				ove_lnx_stats_charge(g_ove_lnx_proc[s].pid, rus);
		}
	}
	/* 2. Build the snapshot: the live Linux procs, then the kernel threads [name]. */
	ove_lnx_stats_begin();
	for (int s = 0; s < OVE_LNX_NSLOT; s++) {
		ove_lnx_proc_t *p = &g_ove_lnx_proc[s];
		if (!p->alive)
			continue;
		char state = (g_ove_lnx_used[s] && !p->sleeping && !p->wait_pending) ? 'R' : 'S';
		ove_lnx_stats_add(p->pid, p->ppid, p->comm, state, ove_lnx_proc_cpu_us(p->pid), 0);
	}
	for (size_t i = 0; i < n; i++) {
		const char *name = ti[i].name ? ti[i].name : "?";
		if (ove_lnx_stats_classify(name) != 0)
			continue; /* idle or a Linux slot thread */
		ove_lnx_stats_add(ove_lnx_kpid_for(name), 0, name, 'S',
				  ti[i].state_times.running_us, 1);
	}
	ove_lnx_stats_set_cpu(idle, busy);
}

/* ---- shared state ---------------------------------------------------------- */
struct ove_lnx_resume_ctx g_ove_lnx_vfork;
ove_lnx_proc_t g_ove_lnx_proc[OVE_LNX_NSLOT];
int g_ove_lnx_used[OVE_LNX_NSLOT];
volatile int g_ove_lnx_active;
/* g_ove_lnx_halt is defined in the syscall layer (reboot(2) sets it) so the
 * host syscall tests link without the run loop; the run loop only observes it. */

static ove_arena_t g_arenas[OVE_LNX_NREG];
static const ove_lnx_run_config_t *g_cfg;
static const struct ove_lnx_engine *g_eng; /* for the dispatch to post coordinator events */

/* Proc-table accessors so the pipe layer can scan all live procs' fds (count a pipe's
 * open read/write ends for EOF / EPIPE) without the syscall layer knowing OVE_LNX_NSLOT. */
ove_lnx_proc_t *ove_lnx_proc_table(void)
{
	return g_ove_lnx_proc;
}
int ove_lnx_proc_nslot(void)
{
	return OVE_LNX_NSLOT;
}

/* Per-slot captured resume context (replaces the single global g_ove_lnx_vfork +
 * the run-loop-local vctx[] — many forks/sleeps/waits can be outstanding at once
 * under the concurrent model). A proc is only ever in ONE of fork/sleep/wait at a
 * time, so one ctx per slot suffices; a vfork child resumes from its PARENT's ctx. */
static struct ove_lnx_resume_ctx g_ctx[OVE_LNX_NSLOT];

static int slot_of(const ove_lnx_proc_t *p)
{
	return (int)(p - g_ove_lnx_proc);
}

/* Capture the post-svc context of frame f into slot s's resume ctx. */
static void capture_ctx(int s, const struct ove_lnx_frame *f)
{
	for (int i = 0; i < 8; i++)
		g_ctx[s].r4_11[i] = f->r[4 + i];
	g_ctx[s].r12 = f->r[12];
	g_ctx[s].lr = f->r[14];
	g_ctx[s].sp = f->r[13];	     /* the seam set r[13] = the pre-svc SP */
	g_ctx[s].pc = f->r[15] | 1u; /* resume after the svc (Thumb) */
}

/* Park the program frame at the spin loop until the coordinator reaps the event,
 * and wake the coordinator (it blocks in event_wait rather than busy-polling). */
static void park_frame(struct ove_lnx_frame *f)
{
	f->r[15] = ((uint32_t)&ove_lnx_park_loop) & ~1u;
	f->xpsr |= (1u << 24);
	if (g_eng && g_eng->event_post)
		g_eng->event_post();
}

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
		park_frame(f); /* the coordinator reaps it */
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
		int target = (int)f->r[0];
		/* halt/poweroff/reboot signal a shutdown to init (pid 1) — SIGUSR1/SIGUSR2/
		 * SIGTERM respectively. init is parked and can't receive it, so honor a
		 * shutdown signal to pid 1 directly as a system halt. */
		if (nr == OVE_LNX_NR_kill && target == 1 && (sig == 10 || sig == 12 || sig == 15)) {
			g_ove_lnx_halt = 1;
			f->r[0] = 0; /* kill() succeeds; the run loop stops next iteration */
			return;
		}
		/* Self-signal (tkill/tgkill, or kill to own pid) is delivered inline. */
		if (nr != OVE_LNX_NR_kill || target == proc->pid) {
			deliver_signal(f, proc, sig, 0);
			return;
		}
		/* Cross-process kill (Phase D3): latch the signal on the target proc; it is
		 * delivered at the target's next syscall boundary (running) or by the
		 * coordinator (parked in sleep/wait/pipe). pid<=0 (process group / all) is
		 * approximated as "every other live userspace proc". */
		f->r[0] = -OVE_LNX_ESRCH;
		for (int t = 0; t < OVE_LNX_NSLOT; t++) {
			ove_lnx_proc_t *tp = &g_ove_lnx_proc[t];
			if (!tp->alive || tp == proc || tp->pid <= 1)
				continue;
			if (target > 0 && tp->pid != target)
				continue;
			tp->pending_sig = sig;
			f->r[0] = 0;
		}
		return;
	}
	if (nr == OVE_LNX_NR_rt_sigreturn || nr == OVE_LNX_NR_sigreturn) {
		sig_restore(f);
		return;
	}
	/* fork/vfork/clone: capture the parent's resume context and ask the coordinator
	 * to spawn a child. The parent is suspended (no thread) through the vfork window
	 * (NOMMU shares the image) until the child execs into its own region or exits. */
	if (nr == OVE_LNX_NR_vfork || nr == OVE_LNX_NR_fork || nr == OVE_LNX_NR_clone) {
		capture_ctx(slot_of(proc), f);
		proc->fork_pending = 1;
		park_frame(f);
		return;
	}

	long r = ove_lnx_syscall(proc, nr, (int32_t)f->r[0], (int32_t)f->r[1], (int32_t)f->r[2],
				 (int32_t)f->r[3], (int32_t)f->r[4], (int32_t)f->r[5]);
	if (r == -OVE_LNX_ENOSYS && g_cfg && g_cfg->on_enosys)
		g_cfg->on_enosys(nr);
	/* nanosleep / blocking wait4: the syscall set the pending flag; capture the
	 * post-svc context (resume the SAME image after the svc) and park. The
	 * coordinator delays/wakes and resumes via spawn_resume(&g_ctx[slot], r0). */
	if (proc->sleep_pending || proc->wait_pending || proc->pipe_wait) {
		capture_ctx(slot_of(proc), f);
		park_frame(f);
		return;
	}
	if (proc->exited || proc->exec_pending) {
		park_frame(f);
		return;
	}
	/* Cross-process signal (Phase D3): another proc's kill() latched a signal on us;
	 * deliver it at this syscall boundary (Linux at-the-boundary async delivery). */
	if (proc->pending_sig) {
		int sig = proc->pending_sig;
		proc->pending_sig = 0;
		deliver_signal(f, proc, sig, r);
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
	g_ove_lnx_proc[sidx].console_poll = g_cfg->console_poll;
	g_ove_lnx_proc[sidx].io_ctx = g_cfg->io_ctx;
	g_ove_lnx_proc[sidx].pid = pid;
	g_ove_lnx_proc[sidx].ppid = ppid;
	/* Concurrent model: this slot is now a live process owning region ridx. */
	g_ove_lnx_proc[sidx].alive = 1;
	g_ove_lnx_proc[sidx].region = ridx;
	g_ove_lnx_proc[sidx].region_owner = 1;
	g_ove_lnx_proc[sidx].vfork_parent_slot = -1;
	/* comm = argv[0] basename (strip the login-shell leading '-') for ps/top. */
	{
		const char *a0 = (argc > 0 && argv && argv[0]) ? argv[0] : "?";
		if (a0[0] == '-')
			a0++;
		const char *base = a0;
		for (const char *s = a0; *s; s++)
			if (*s == '/')
				base = s + 1;
		size_t cl = strlen(base);
		if (cl >= sizeof(g_ove_lnx_proc[sidx].comm))
			cl = sizeof(g_ove_lnx_proc[sidx].comm) - 1;
		memcpy(g_ove_lnx_proc[sidx].comm, base, cl);
		g_ove_lnx_proc[sidx].comm[cl] = '\0';
	}
	ove_lnx_proc_set_rootfs(&g_ove_lnx_proc[sidx], g_cfg->rootfs, g_cfg->rootfs_count);
	uint8_t *stack_lo = rw + OVE_LNX_PROG_ARENA_SIZE;
	void *sp = ove_lnx_setup_stack(stack_lo, (size_t)(rw_end - stack_lo), argc, argv, NULL);
	if (!sp)
		return -1;
	return eng->spawn_launch(sidx, ridx, &prog, (void *)prog.entry, sp, stack_lo);
}

/* A child (cpid, status) exited: hand it to its parent (ppid). Wake a parent blocked
 * in wait4 (resume returning cpid + write *status), else queue the zombie for a later
 * wait4. Decrements the parent's live-children count either way. */
static void reap_to_parent(const struct ove_lnx_engine *eng, int ppid, int cpid, int status)
{
	int pslot = -1;
	for (int t = 0; t < OVE_LNX_NSLOT; t++)
		if (g_ove_lnx_proc[t].alive && g_ove_lnx_proc[t].pid == ppid) {
			pslot = t;
			break;
		}
	if (pslot < 0)
		return;
	ove_lnx_proc_t *par = &g_ove_lnx_proc[pslot];
	if (par->live_children > 0)
		par->live_children--;
	if (par->wait_pending && (par->wait_pid <= 0 || par->wait_pid == cpid)) {
		if (par->wait_status_p)
			*(int *)(uintptr_t)par->wait_status_p = (status & 0xff) << 8;
		par->wait_pending = 0;
		if (g_ove_lnx_used[pslot]) /* abort the parked-waiter spin thread first */
			eng->abort_slot(pslot);
		eng->spawn_resume(pslot, par->region, &g_ctx[pslot], cpid);
	} else if (par->child_count < OVE_LNX_MAX_CHILD) {
		par->child_pid[par->child_count] = cpid;
		par->child_status[par->child_count] = status;
		par->child_count++;
	}
}

int ove_lnx_run_common(const struct ove_lnx_engine *eng, const ove_lnx_run_config_t *cfg,
		       const char *path, int argc, const char *const argv[])
{
	if (!eng || !cfg || !cfg->rootfs || !path || argc < 1 || !argv)
		return OVE_LNX_RUN_ELAUNCH;
	g_cfg = cfg;
	g_eng = eng;
	for (int i = 0; i < OVE_LNX_NSLOT; i++) {
		g_ove_lnx_used[i] = 0;
		g_ove_lnx_proc[i].alive = 0;
	}
	g_pending_sig = 0;
	g_tty_isig = 1;
	ove_lnx_stats_reset();
	g_sig_save.active = 0;

	int bb = -1;
	for (int i = 0; i < cfg->rootfs_count; i++)
		if (strcmp(cfg->rootfs[i].path, path) == 0) {
			bb = i;
			break;
		}
	if (bb < 0 || !cfg->rootfs[bb].data)
		return OVE_LNX_RUN_ELAUNCH;

	/* Concurrent process model: the run loop COORDINATES the live process SET
	 * (g_ove_lnx_proc[*].alive). Each live proc owns a region + an RTOS thread for
	 * its lifetime; a vfork parent resumes the instant its child execs into its own
	 * region (or exits) so the two co-run. rowner[r] = the slot owning region r. */
	int rowner[OVE_LNX_NREG]; /* slot that owns each region, or -1 */
	for (int r = 0; r < OVE_LNX_NREG; r++)
		rowner[r] = -1;

	g_ove_lnx_active = 1;
	g_ove_lnx_halt = 0;
	rowner[0] = 0;
	if (launch(eng, 0, 0, cfg->rootfs[bb].data, cfg->rootfs[bb].size, 1, 0, argc, argv) != 0) {
		g_ove_lnx_active = 0;
		return OVE_LNX_RUN_ELAUNCH;
	}

	int rc = OVE_LNX_RUN_ETIMEOUT;
	int next_pid = 2;
	int idle = 0;
	uint64_t last_refresh_us = 0;
	for (;;) {
		if (g_ove_lnx_halt) { /* reboot(2)/poweroff: stop the whole system */
			rc = 0;
			break;
		}

		/* Claim ONE pending event under the crit (flags are atomic ints; the brief
		 * masked window keeps a preempting program svc from racing the read/clear).
		 * Act on it OUTSIDE the crit — abort/spawn/launch may yield. */
		int es = -1, et = 0;
		enum { EV_EXIT = 1, EV_EXEC, EV_FORK, EV_SLEEP, EV_WAITPARK, EV_PIPE };
		eng->crit_enter();
		for (int s = 0; s < OVE_LNX_NSLOT; s++) {
			ove_lnx_proc_t *p = &g_ove_lnx_proc[s];
			if (!p->alive)
				continue;
			if (p->exited) {
				es = s;
				et = EV_EXIT;
				break;
			}
			if (p->exec_pending) {
				es = s;
				et = EV_EXEC;
				break;
			}
			if (p->fork_pending) {
				p->fork_pending = 0;
				es = s;
				et = EV_FORK;
				break;
			}
			if (p->sleep_pending) {
				p->sleep_pending = 0;
				es = s;
				et = EV_SLEEP;
				break;
			}
			if (p->wait_pending && g_ove_lnx_used[s]) {
				es = s;
				et = EV_WAITPARK;
				break;
			}
			if (p->pipe_wait && g_ove_lnx_used[s]) {
				es = s;
				et = EV_PIPE;
				break;
			}
		}
		eng->crit_exit();

		if (et ==
		    EV_FORK) { /* spawn a child sharing the parent's region; suspend the parent. */
			ove_lnx_proc_t *par = &g_ove_lnx_proc[es];
			int c = -1;
			for (int s = 0; s < OVE_LNX_NSLOT; s++)
				if (!g_ove_lnx_proc[s].alive) {
					c = s;
					break;
				}
			if (c < 0) { /* no free slot: fail the fork (parent gets -ENOMEM). */
				eng->abort_slot(es);
				eng->spawn_resume(es, par->region, &g_ctx[es], -OVE_LNX_ENOMEM);
				idle = 0;
				continue;
			}
			ove_lnx_proc_t *ch = &g_ove_lnx_proc[c];
			*ch = *par; /* vfork shares the image + region */
			ch->pid = next_pid++;
			ch->ppid = par->pid;
			ch->exited = ch->exec_pending = ch->fork_pending = 0;
			ch->sleep_pending = ch->wait_pending = ch->sleeping = 0;
			ch->pipe_wait = 0;
			ch->pending_sig = 0;
			ch->child_count = ch->live_children = 0;
			ch->alive = 1;
			ch->region = par->region;
			ch->region_owner =
				0; /* shares the parent's region during the vfork window */
			ch->vfork_parent_slot =
				es; /* resume the parent when this child execs/exits */
			par->live_children++;
			eng->abort_slot(es); /* suspend the parent (no thread) */
			eng->spawn_resume(c, ch->region, &g_ctx[es],
					  0); /* child returns 0 from fork */
			idle = 0;
			continue;
		}

		if (et ==
		    EV_EXEC) { /* the child gets its own region → resume any vfork parent NOW. */
			ove_lnx_proc_t *p = &g_ove_lnx_proc[es];
			int idx = p->exec_file_idx, eargc = p->exec_argc;
			static char args[OVE_LNX_EXEC_ARGBUF];
			static const char *ptrs[OVE_LNX_EXEC_MAXARGS + 1];
			size_t off = 0;
			for (int j = 0; j < eargc; j++) {
				size_t n = strlen(p->exec_argv[j]) + 1;
				memcpy(args + off, p->exec_argv[j], n);
				ptrs[j] = args + off;
				off += n;
			}
			ptrs[eargc] = NULL;
			int nr = -1;
			for (int r = 0; r < OVE_LNX_NREG; r++)
				if (rowner[r] < 0) {
					nr = r;
					break;
				}
			int pid = p->pid, ppid = p->ppid, vp = p->vfork_parent_slot;
			if (nr <
			    0) { /* region exhaustion: kill THIS proc, do NOT tear down init. */
				eng->abort_slot(es);
				if (p->region_owner && rowner[p->region] == es)
					rowner[p->region] = -1;
				p->alive = 0;
				g_ove_lnx_used[es] = 0;
				if (vp >= 0)
					eng->spawn_resume(vp, g_ove_lnx_proc[vp].region, &g_ctx[vp],
							  pid);
				reap_to_parent(eng, ppid, pid, 139);
				idle = 0;
				continue;
			}
			if (vp >=
			    0) { /* the child leaves the parent's region → the parent co-runs. */
				p->vfork_parent_slot = -1;
				eng->spawn_resume(vp, g_ove_lnx_proc[vp].region, &g_ctx[vp], pid);
			}
			/* fds + cwd survive execve: preserve across the relaunch (launch re-inits). */
			ove_lnx_fd_t saved_fds[OVE_LNX_MAX_FDS];
			char saved_cwd[OVE_LNX_PATH_MAX];
			memcpy(saved_fds, p->fds, sizeof(saved_fds));
			memcpy(saved_cwd, p->cwd, sizeof(saved_cwd));
			if (p->region_owner &&
			    rowner[p->region] == es) /* free the old owned region */
				rowner[p->region] = -1;
			rowner[nr] = es;
			eng->abort_slot(es);
			if (launch(eng, es, nr, cfg->rootfs[idx].data, cfg->rootfs[idx].size, pid,
				   ppid, eargc, ptrs) != 0) {
				rowner[nr] = -1;
				g_ove_lnx_proc[es].alive = 0;
				g_ove_lnx_used[es] = 0;
				reap_to_parent(eng, ppid, pid, 127);
				idle = 0;
				continue;
			}
			memcpy(g_ove_lnx_proc[es].fds, saved_fds, sizeof(saved_fds));
			memcpy(g_ove_lnx_proc[es].cwd, saved_cwd, sizeof(saved_cwd));
			idle = 0;
			continue;
		}

		if (et ==
		    EV_EXIT) { /* reap: abort thread, free region, wake parent/queue zombie. */
			ove_lnx_proc_t *p = &g_ove_lnx_proc[es];
			int cpid = p->pid, status = p->exit_status, vp = p->vfork_parent_slot,
			    ppid = p->ppid;
			eng->abort_slot(es);
			if (p->region_owner && rowner[p->region] == es)
				rowner[p->region] = -1;
			p->alive = 0;
			g_ove_lnx_used[es] = 0;
			if (es == 0) { /* init exited → the system is done */
				rc = status;
				break;
			}
			if (vp >=
			    0) /* fork-without-exec: the suspended parent resumes (vfork returns) */
				eng->spawn_resume(vp, g_ove_lnx_proc[vp].region, &g_ctx[vp], cpid);
			reap_to_parent(eng, ppid, cpid, status);
			idle = 0;
			continue;
		}

		if (et ==
		    EV_SLEEP) { /* park the slot for the nanosleep duration (deadline below). */
			eng->abort_slot(es);
			g_ove_lnx_proc[es].sleeping = 1;
			idle = 0;
			continue;
		}
		if (et ==
		    EV_WAITPARK) { /* free the blocked waiter's spin thread until a child exits. */
			eng->abort_slot(es);
			idle = 0;
			continue;
		}
		if (et == EV_PIPE) { /* free the spin thread; the retry below resumes it. */
			eng->abort_slot(es);
			idle = 0;
			continue;
		}

		/* No pending event: resume any sleeper whose deadline passed; assess liveness. */
		uint64_t now = 0;
		ove_time_get_us(&now);
		int progress = 0, any_alive = 0, any_busy = 0, any_pipe_wait = 0;
		for (int s = 0; s < OVE_LNX_NSLOT; s++) {
			ove_lnx_proc_t *p = &g_ove_lnx_proc[s];
			if (!p->alive)
				continue;
			any_alive = 1;
			if (g_ove_lnx_used[s])
				any_busy = 1;
			if (p->pipe_wait)
				any_pipe_wait = 1;
			/* Cross-process signal (D3) to a parked, blocked proc: the dispatch can't
			 * deliver (no live thread), so the coordinator does. SIG_IGN is dropped;
			 * otherwise terminate (default action — a custom handler on a blocked proc
			 * is approximated as terminate). EV_EXIT reaps it next pass. */
			if (p->pending_sig && !g_ove_lnx_used[s] &&
			    (p->sleeping || p->wait_pending || p->pipe_wait)) {
				int sig = p->pending_sig;
				p->pending_sig = 0;
				if (p->sig_handler[sig] != OVE_LNX_SIG_IGN) {
					p->exited = 1;
					p->exit_status = 128 + sig;
					p->sleeping = p->wait_pending = p->pipe_wait = 0;
					progress = 1;
				}
			}
			if (p->sleeping) {
				any_busy = 1;
				if (now >= p->sleep_until_us) {
					p->sleeping = 0;
					eng->spawn_resume(s, p->region, &g_ctx[s], 0);
					progress = 1;
				}
			}
			/* Blocked pipe I/O: retry now that a peer may have drained/filled the
			 * ring (or closed its end → EOF/EPIPE); resume the proc when it completes. */
			if (p->pipe_wait && !g_ove_lnx_used[s]) {
				long r = ove_lnx_pipe_retry(p);
				if (r != -OVE_LNX_EAGAIN) {
					p->pipe_wait = 0;
					if (r == -OVE_LNX_EPIPE &&
					    p->sig_handler[OVE_LNX_SIGPIPE] != OVE_LNX_SIG_IGN) {
						/* broken pipe + default SIGPIPE → terminate the
						 * writer; the EV_EXIT pass reaps it (no live thread). */
						p->exited = 1;
						p->exit_status = 128 + OVE_LNX_SIGPIPE;
					} else {
						eng->spawn_resume(s, p->region, &g_ctx[s], r);
					}
					progress = 1;
				}
			}
		}
		if (!any_alive) {
			rc = 0;
			break;
		}
		if (progress) {
			idle = 0;
			continue;
		}
		/* Idle watchdog: trip only when nothing is runnable (a true deadlock — all
		 * procs blocked with no waker); a running or sleeping proc resets it. */
		if (any_busy)
			idle = 0;
		else if (++idle > 20000) {
			rc = OVE_LNX_RUN_ETIMEOUT;
			break;
		}
		if (now - last_refresh_us >= 200000ull) {
			last_refresh_us = now;
			refresh_stats();
		}
		/* Block until a program parks (event_post) or the timeout (sleeper deadlines
		 * + snapshot refresh). NOT a busy 1ms poll — that would preempt running
		 * programs every tick and reset their time-slice, starving a fg command
		 * while a CPU-bound background job runs. A short timeout while a pipe is
		 * blocked keeps `a | b` snappy (the peer's read/write doesn't post an event). */
		eng->event_wait(any_pipe_wait ? 5 : 50);
	}
	/* Tear down any still-running slot tasks so a subsequent ove_lnx_run() starts
	 * clean and no leaked task starves the next program. */
	for (int i = 0; i < OVE_LNX_NSLOT; i++)
		if (g_ove_lnx_used[i])
			eng->abort_slot(i);
	g_ove_lnx_active = 0;
	return rc;
}
