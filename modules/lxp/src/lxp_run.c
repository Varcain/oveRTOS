/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Engine-agnostic Linux-personality run loop + svc dispatch + signal delivery,
 * shared by the Zephyr / FreeRTOS / NuttX seams (see lxp_run.h). The
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

#include "lxp/lxp_arena.h"
#include "lxp/lxp_types.h"
#include "lxp/lxp_types.h"
#include "lxp/lxp_seam.h"
#include "lxp/lxp_stats.h"
#if defined(LXP_ENABLE_DEV)
#include "lxp/lxp_dev.h" /* device-layer park/retry + autoreg + tick + kick */
#endif
#if defined(LXP_ENABLE_NET)
#include "lxp/lxp_net.h" /* socket-layer park/retry + fork/exit fd lifecycle */
#endif
#if defined(LXP_ENABLE_NETFS)
#include "lxp/lxp_netfs.h" /* remote-fs park/retry + init/pump + fork/exit lifecycle */
#endif
#if defined(LXP_ENABLE_PTY)
#include "lxp/lxp_pty.h" /* pty-layer park/retry (lxp_pty_retry) */
#endif

/* Declare a memory-mapped rootfs image window [base, base+len) so the coordinator task can
 * read it safely on the target.  Default: no-op — an ordinary CPU view of the window is already
 * correct (a RAM-backed rootfs, or an engine that covers the window with a global MPU region,
 * e.g. NuttX region 4 / Zephyr).  The FreeRTOS seam strong-overrides this on the STM32F746
 * (QUADSPI-XIP rootfs + M7 D-cache): it installs a bounded, non-cacheable MPU region for the
 * calling task so cache line-fill bursts + speculative prefetch never reach the memory-mapped
 * NOR.  A weak definition so only the engines that need it provide one. */
__attribute__((weak)) void lxp_rootfs_window(const void *base, size_t len)
{
	(void)base;
	(void)len;
}

/* Engine-common weak no-op; the FreeRTOS backend strong-overrides this on the STM32F746 where the
 * guest writes cacheable SDRAM but the coordinator reads it uncached (D-cache coherency). CLEANS
 * (writes back) the guest's cache lines so a subsequent uncached coordinator read sees them. */
__attribute__((weak)) void lxp_guest_flush(const void *base, size_t len)
{
	(void)base;
	(void)len;
}

/* Engine-common weak no-op; FreeRTOS strong-overrides it. INVALIDATES the guest's D-cache lines over
 * [base, len) so the guest's next read misses and refills from SDRAM — used after the coordinator has
 * written guest memory through its uncached view (vfork restore). Invalidate, NOT clean: a clean would
 * write the guest's stale lines back OVER what the coordinator just put in SDRAM. */
__attribute__((weak)) void lxp_guest_invalidate(const void *base, size_t len)
{
	(void)base;
	(void)len;
}

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
	struct lxp_thread_info ti[LXP_MAX_KTHREAD];
	size_t n = 0;
	if (lxp_thread_list(ti, LXP_MAX_KTHREAD, &n) != LXP_OK)
		n = 0; /* no host introspection: /proc shows only the Linux procs */

	/* 1. Charge each live Linux thread's CPU to its proc (slot from the name). */
	uint64_t idle = 0, busy = 0;
	for (size_t i = 0; i < n; i++) {
		const char *name = ti[i].name ? ti[i].name : "?";
		uint64_t rus = ti[i].state_times.running_us;
		int cls = lxp_stats_classify(name);
		if (cls == 1) {
			idle += rus;
			continue;
		}
		busy += rus;
		if (cls == 2) {
			int s = lnx_slot_of_name(name);
			if (s >= 0 && s < LXP_NSLOT && g_lxp_proc[s].alive)
				lxp_stats_charge(g_lxp_proc[s].pid, rus);
		}
	}
	/* 2. Build the snapshot: the live Linux procs, then the kernel threads [name]. */
	lxp_stats_begin();
	for (int s = 0; s < LXP_NSLOT; s++) {
		lxp_proc_t *p = &g_lxp_proc[s];
		if (!p->alive)
			continue;
		char state = (g_lxp_used[s] && !p->sleeping && !p->wait_pending) ? 'R' : 'S';
		lxp_stats_add(p->pid, p->ppid, p->comm, state, lxp_proc_cpu_us(p->pid), 0);
	}
	for (size_t i = 0; i < n; i++) {
		const char *name = ti[i].name ? ti[i].name : "?";
		if (lxp_stats_classify(name) != 0)
			continue; /* idle or a Linux slot thread */
		lxp_stats_add(lxp_kpid_for(name), 0, name, 'S',
				  ti[i].state_times.running_us, 1);
	}
	lxp_stats_set_cpu(idle, busy);
}

/* ---- shared state ---------------------------------------------------------- */
struct lxp_resume_ctx g_lxp_vfork;
lxp_proc_t g_lxp_proc[LXP_NSLOT];
int g_lxp_used[LXP_NSLOT];
volatile int g_lxp_active;
/* g_lxp_halt is defined in the syscall layer (reboot(2) sets it) so the
 * host syscall tests link without the run loop; the run loop only observes it. */

static lxp_arena_t g_arenas[LXP_NREG];
/* vfork data isolation: a snapshot of the shared arena's allocator metadata, taken when a vfork
 * child is spawned (keyed by the child's slot) and restored when it execs/exits — the region+dyn_pool
 * BYTES are snapshotted into a spare region, but g_arenas[] lives in coordinator memory. */
static lxp_arena_t g_snap_arena[LXP_NSLOT];
static const lxp_run_config_t *g_cfg;
static const struct lxp_engine *g_eng; /* for the dispatch to post coordinator events */

/* ---- OS-service hooks routed through the engine ops ------------------------
 * The personality core calls these instead of the host's ove_time_* / cache
 * primitives, so it carries no direct dependency on any particular OS. The seam
 * (host adapter) fills the ops; g_eng is live for the duration of a run. */
int lxp_time_us(uint64_t *out)
{
	if (g_eng && g_eng->time_us)
		return g_eng->time_us(out);
	*out = 0;
	return LXP_ERR_NOT_SUPPORTED;
}
int lxp_time_ns(uint64_t *out)
{
	if (g_eng && g_eng->time_ns)
		return g_eng->time_ns(out);
	*out = 0;
	return LXP_ERR_NOT_SUPPORTED;
}
void lxp_cache_clean(const void *base, size_t len)
{
	if (g_eng && g_eng->cache_clean)
		g_eng->cache_clean(base, len);
}
void lxp_cache_invalidate(const void *base, size_t len)
{
	if (g_eng && g_eng->cache_invalidate)
		g_eng->cache_invalidate(base, len);
}
int lxp_thread_list(struct lxp_thread_info *out, size_t max_count, size_t *actual_count)
{
	if (g_eng && g_eng->thread_list)
		return g_eng->thread_list(out, max_count, actual_count);
	if (actual_count)
		*actual_count = 0;
	return LXP_ERR_NOT_SUPPORTED;
}

/* The cpio data region [lo, hi): the embedded rootfs files' bytes. A dynamic FDPIC proc now runs
 * ALL its code — busybox.so + ld.so + libc.so text, shared in-place — straight from here, so a
 * PC-discriminating seam (NuttX) must count a cpio PC as "in a program" when routing the svc.
 * NULL until a run starts. */
const uint8_t *g_lxp_rootfs_lo;
const uint8_t *g_lxp_rootfs_hi;

/* access_ok (ove_linux_syscall.c) asks for the shared read-only rootfs span so a read-source user
 * pointer may point into a program's .rodata (shared in-place from the cpio). Strong override of the
 * weak stub in the syscall layer. */
void lxp_rootfs_bounds(uintptr_t *lo, uintptr_t *hi)
{
	*lo = (uintptr_t)g_lxp_rootfs_lo;
	*hi = (uintptr_t)g_lxp_rootfs_hi;
}

/* Proc-table accessors so the pipe layer can scan all live procs' fds (count a pipe's
 * open read/write ends for EOF / EPIPE) without the syscall layer knowing LXP_NSLOT. */
lxp_proc_t *lxp_proc_table(void)
{
	return g_lxp_proc;
}
int lxp_proc_nslot(void)
{
	return LXP_NSLOT;
}

#if defined(LXP_ENABLE_DEV)
/* Wake the coordinator so it retries parked device I/O at once (a driver calls this
 * from its data-ready path). Strong override of the weak no-op in the device core
 * — that stub is used only by the host test, which links no run loop. */
void lxp_dev_kick(void)
{
	if (g_eng && g_eng->event_post)
		g_eng->event_post();
}
#endif

#if defined(LXP_ENABLE_NET)
/* Wake the coordinator so it retries parked socket I/O at once — the network RX task calls
 * this after delivering a batch of frames to the stack, so a parked recv/connect/accept
 * resumes the instant its data/ACK lands instead of on the next ≤5 ms retry tick. Mirrors
 * lxp_dev_kick; only ever called with LXP_ENABLE_NET (⇒ OVE_LINUX) set, so this
 * run loop is always linked and no weak no-op is needed. */
void lxp_sock_kick(void)
{
	if (g_eng && g_eng->event_post)
		g_eng->event_post();
}
#endif

#if defined(LXP_ENABLE_NETFS)
/* Wake the coordinator so it pumps the 9P transport at once — the eth RX task calls this
 * after delivering frames, so a parked netfs op resumes the instant its reply lands. */
void lxp_netfs_kick(void)
{
	if (g_eng && g_eng->event_post)
		g_eng->event_post();
}
#endif


/* Per-slot captured resume context (replaces the single global g_lxp_vfork +
 * the run-loop-local vctx[] — many forks/sleeps/waits can be outstanding at once
 * under the concurrent model). A proc is only ever in ONE of fork/sleep/wait at a
 * time, so one ctx per slot suffices; a vfork child resumes from its PARENT's ctx. */
static struct lxp_resume_ctx g_ctx[LXP_NSLOT];

/* Per-slot FDPIC runtime load addresses, exported (non-static) for SOURCE-LEVEL GDB DEBUGGING of
 * the userspace program. An FDPIC exec is loaded at runtime addresses (the loadmap relocates each
 * segment independently), so the on-disk ELF's link addresses don't match memory. A GDB helper
 * (config/scripts/ove-fdpic-gdb.py) reads this table and `add-symbol-file <elf> -o <text_base>`s the
 * program, then walks the exec's _DYNAMIC[DT_DEBUG] rendezvous (populated by ld.so once it has run)
 * to auto-load ld.so + every shared library at its own FDPIC bias. comm[] (in g_lxp_proc) names
 * the program; text_base/data_base are the loadmap-relocated bases of its text/data segments. */
struct lxp_dbg_s {
	uintptr_t text_base; /* runtime base of the program's text (shared in-place from the cpio) */
	uintptr_t data_base; /* runtime base of the program's RW data (in the slot's region) */
	uintptr_t entry;     /* the program's own entry (AT_ENTRY), not ld.so's */
	uintptr_t dynamic;   /* runtime addr of the exec's _DYNAMIC → DT_DEBUG → the ld.so link-map chain */
	uintptr_t interp_base; /* ld.so's text base (0 for a static exec); auto-solib loads ld.so there */
};
struct lxp_dbg_s g_lxp_dbg[LXP_NSLOT];

static int slot_of(const lxp_proc_t *p)
{
	return (int)(p - g_lxp_proc);
}

/* Capture the post-svc context of frame f into slot s's resume ctx. */
static void capture_ctx(int s, const struct lxp_frame *f)
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
static void park_frame(struct lxp_frame *f)
{
	f->r[15] = ((uint32_t)&lxp_park_loop) & ~1u;
	f->xpsr |= (1u << 24);
	if (g_eng && g_eng->event_post)
		g_eng->event_post();
}

/* Saved interrupted context for an in-flight signal handler. r4-r11 are NOT
 * saved (a C handler preserves them, so they are already correct at sigreturn).
 * No nesting (one handler at a time). */
/* Per-slot, NOT a single global: pthreads (LinuxThreads) deliver restart signals to
 * several threads concurrently — each slot's handler frame must save/restore its own
 * interrupted context, or one thread's sigreturn clobbers another's. */
static struct sig_save_s {
	uint32_t r0, r1, r2, r3, r9, r12, lr, pc, xpsr;
	int active;
} g_sig_save[LXP_NSLOT];

static volatile int g_tty_isig = 1;
static volatile int g_pending_sig;

int lxp_tty_isig(void)
{
	return g_tty_isig;
}

void lxp_post_signal(int sig)
{
	g_pending_sig = sig;
}

void lxp_park_loop(void)
{
	for (;;) {
	}
}

/* ---- signal delivery (over the uniform frame) ------------------------------ */
/* Resolve a handler + restorer for delivery. FDPIC: sa_handler/sa_restorer are function DESCRIPTORS
 * {entry, GOT} — deref, since the handler may live in a different module (e.g. libpthread) than the
 * interrupted code and needs its own r9=GOT. Non-FDPIC (e.g. the posix host test): raw entries, no
 * GOT change. */
static void resolve_handler(const lxp_proc_t *proc, int sig, uintptr_t *entry, uint32_t *got,
			    uintptr_t *restorer)
{
	uintptr_t h = proc->sig_handler[sig];
	uintptr_t r = proc->sig_restorer[sig];
	if (proc->is_fdpic) {
		*entry = ((const uint32_t *)h)[0];
		*got = ((const uint32_t *)h)[1];
		*restorer = ((const uint32_t *)r)[0];
	} else {
		*entry = h;
		*got = 0;
		*restorer = r;
	}
}

/* Is signal `sig` effectively ignored for `proc`? True for SIG_IGN, or SIG_DFL of a
 * signal whose default action is "ignore" (SIGCHLD). Such a signal is swallowed by the
 * coordinator: it neither runs a handler nor terminates a parked proc — a parent must
 * not die because a child exited. */
static int sig_swallowed(const lxp_proc_t *proc, int sig)
{
	uintptr_t h = proc->sig_handler[sig];
	if (h == LXP_SIG_IGN)
		return 1;
	if (h == LXP_SIG_DFL && sig == LXP_SIGCHLD)
		return 1;
	return 0;
}

/* Deliver signal `sig` to `proc`; `ret` is the interrupted syscall's result
 * (0 for a kill/tkill, -EINTR for a console-interrupted read). */
static void deliver_signal(struct lxp_frame *f, lxp_proc_t *proc, int sig, long ret)
{
	if (sig < 1 || sig >= LXP_NSIG) {
		f->r[0] = (uint32_t)-LXP_EINVAL;
		return;
	}
	uintptr_t h = proc->sig_handler[sig];
	if (h == LXP_SIG_IGN || (h == LXP_SIG_DFL && sig == LXP_SIGCHLD)) {
		f->r[0] = (uint32_t)ret; /* SIG_IGN, or a default-ignore signal (SIGCHLD) */
		return;
	}
	if (h == LXP_SIG_DFL) {
		proc->exited = 1;
		proc->exit_status = 128 + sig;
		park_frame(f); /* the coordinator reaps it */
		return;
	}
	struct sig_save_s *sv = &g_sig_save[slot_of(proc)];
	sv->r0 = (uint32_t)ret;
	sv->r1 = f->r[1];
	sv->r2 = f->r[2];
	sv->r3 = f->r[3];
	sv->r9 = f->r[9]; /* FDPIC GOT of the interrupted code — clobbered below, restored at sigreturn */
	sv->r12 = f->r[12];
	sv->lr = f->r[14];
	sv->pc = f->r[15];
	sv->xpsr = f->xpsr;
	sv->active = 1;
	uintptr_t entry, restorer;
	uint32_t got;
	resolve_handler(proc, sig, &entry, &got, &restorer);
	if (proc->is_fdpic)
		f->r[9] = got;			 /* FDPIC: r9 = the handler's own GOT */
	f->r[15] = entry & ~1u;			 /* pc -> handler entry (Thumb via xPSR.T) */
	f->r[0] = (uint32_t)sig;		 /* r0 = signo */
	f->r[14] = restorer | 1u;		 /* lr -> sa_restorer */
	f->xpsr |= (1u << 24);
}

/* rt_sigreturn: restore the context saved at delivery. */
static void sig_restore(struct lxp_frame *f, const lxp_proc_t *proc)
{
	struct sig_save_s *sv = &g_sig_save[slot_of(proc)];
	if (!sv->active)
		return;
	f->r[0] = sv->r0;
	f->r[1] = sv->r1;
	f->r[2] = sv->r2;
	f->r[3] = sv->r3;
	f->r[9] = sv->r9; /* FDPIC GOT: the handler ran with its own r9; restore the interrupted code's */
	f->r[12] = sv->r12;
	f->r[14] = sv->lr;
	f->r[15] = sv->pc & ~1u;
	f->xpsr = sv->xpsr;
	sv->active = 0;
}

/* ---- the syscall dispatch body --------------------------------------------- */
void lxp_dispatch(struct lxp_frame *f, lxp_proc_t *proc)
{
	long nr = (long)(int32_t)f->r[7];

	/* Track the tty ISIG mode so a console ^C knows whether to raise SIGINT
	 * (canonical) or pass ^C through (the shell's raw line editor). */
	if (nr == LXP_NR_ioctl) {
		unsigned long cmd = f->r[1];
		if (cmd == LXP_TCSETS || cmd == LXP_TCSETSW || cmd == LXP_TCSETSF) {
			const lxp_termios *t = (const lxp_termios *)(uintptr_t)f->r[2];
			if (t)
				g_tty_isig = (t->c_lflag & LXP_ISIG) ? 1 : 0;
		}
	}
	if (nr == LXP_NR_kill || nr == LXP_NR_tkill || nr == LXP_NR_tgkill) {
		int sig = (nr == LXP_NR_tgkill) ? (int)f->r[2] : (int)f->r[1];
		int target = (int)f->r[0];
		/* halt/poweroff/reboot signal a shutdown to init (pid 1) — SIGUSR1/SIGUSR2/
		 * SIGTERM respectively. init is parked and can't receive it, so honor a
		 * shutdown signal to pid 1 directly as a system halt. */
		if (nr == LXP_NR_kill && target == 1 && (sig == 10 || sig == 12 || sig == 15)) {
			g_lxp_halt = 1;
			f->r[0] = 0; /* kill() succeeds; the run loop stops next iteration */
			return;
		}
		/* Self-signal (tkill/tgkill, or kill to own pid) is delivered inline. */
		if (nr != LXP_NR_kill || target == proc->pid) {
			deliver_signal(f, proc, sig, 0);
			return;
		}
		/* Cross-process kill (Phase D3): latch the signal on the target proc; it is
		 * delivered at the target's next syscall boundary (running) or by the
		 * coordinator (parked in sleep/wait/pipe). pid<=0 (process group / all) is
		 * approximated as "every other live userspace proc". */
		f->r[0] = -LXP_ESRCH;
		for (int t = 0; t < LXP_NSLOT; t++) {
			lxp_proc_t *tp = &g_lxp_proc[t];
			if (!tp->alive || tp == proc || tp->pid <= 1)
				continue;
			if (target > 0 && tp->pid != target)
				continue;
			tp->pending_sig = sig;
			f->r[0] = 0;
		}
		/* Wake the coordinator NOW so it delivers the signal at once (the LinuxThreads
		 * restart) instead of at its next ~poll-interval tick — otherwise every thread
		 * wakeup costs up to one event_wait timeout. */
		if (f->r[0] == 0 && g_eng && g_eng->event_post)
			g_eng->event_post();
		return;
	}
	if (nr == LXP_NR_rt_sigreturn || nr == LXP_NR_sigreturn) {
		sig_restore(f, proc);
		return;
	}
	/* fork/vfork/clone: capture the parent's resume context and ask the coordinator
	 * to spawn a child. The parent is suspended (no thread) through the vfork window
	 * (NOMMU shares the image) until the child execs into its own region or exits. */
	if (nr == LXP_NR_vfork || nr == LXP_NR_fork || nr == LXP_NR_clone) {
		/* clone(CLONE_VM) is a pthread: the child shares the parent's region for life and
		 * runs on its own stack (clone arg r1), and the parent CO-RUNS (gets the child tid)
		 * — unlike fork/vfork, which suspend the parent until the child execs/exits. */
		if (nr == LXP_NR_clone && ((uint32_t)f->r[0] & LXP_CLONE_VM)) {
			proc->clone_is_thread = 1;
			proc->clone_child_stack = f->r[1];
		}
		capture_ctx(slot_of(proc), f);
		proc->fork_pending = 1;
		park_frame(f);
		return;
	}

	long r = lxp_syscall(proc, nr, (int32_t)f->r[0], (int32_t)f->r[1], (int32_t)f->r[2],
				 (int32_t)f->r[3], (int32_t)f->r[4], (int32_t)f->r[5]);
	/* Suppress the console diagnostic for syscalls we deliberately don't implement but the guest
	 * probes and gracefully falls back on: getdents(141)→getdents64, socket(281)→no networking. */
	if (r == -LXP_ENOSYS && g_cfg && g_cfg->on_enosys && nr != 141 && nr != 281)
		g_cfg->on_enosys(nr);
	/* nanosleep / blocking wait4: the syscall set the pending flag; capture the
	 * post-svc context (resume the SAME image after the svc) and park. The
	 * coordinator delays/wakes and resumes via spawn_resume(&g_ctx[slot], r0). */
	if (proc->sleep_pending || proc->wait_pending || proc->pipe_wait || proc->dev_wait ||
	    proc->sock_wait || proc->netfs_wait || proc->pty_wait || proc->sigsuspend_pending ||
	    proc->console_wait) {
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
/* Load an FDPIC ELF into region ridx + set up slot sidx's proc, then spawn it. @p remote_exec:
 * the image is a RAM staging buffer (a program off the remote mount), so the exec's own text is
 * copied into the region and the region is mapped executable (RWX) — gated by the caller. */
static int launch(const struct lxp_engine *eng, int sidx, int ridx, const uint8_t *data,
		  size_t len, int pid, int ppid, int argc, const char *const argv[], int remote_exec)
{
	uint8_t *region = eng->region(ridx);
	lxp_flat_t prog;
	/* Every personality program is an FDPIC ELF (0x7f'ELF', ELFOSABI_ARM_FDPIC); reject
	 * anything else. spawn_launch reads prog.is_fdpic / prog.got to put the GOT base in r9. */
	if (!(len >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F'))
		return -1;
	/* The loader reads the FDPIC ELF from `data` — on the STM32F746 that points into the
	 * QUADSPI-mapped NOR (0x90000000).  Correctness of that read is a memory-attribute concern,
	 * not a timing one: the coordinator reads the NOR through a bounded, non-cacheable MPU region
	 * (lxp_rootfs_window), so no D-cache burst or speculative prefetch can garble it and a
	 * context switch mid-load is harmless.  No preemption masking needed. */
	int lrc = lxp_loader_load_fdpic(&prog, data, len, region, LXP_PROG_REGION_SIZE, 0,
					remote_exec);
	if (lrc != LXP_OK)
		return -1;

	/* FDPIC dynamic exec (DT_NEEDED): load the interpreter ld.so just past the exec in the
	 * region, build its loadmap, and enter IT (not the program) — r7 = the exec loadmap,
	 * r8 = ld.so's loadmap, r9 = ld.so's GOT, AT_ENTRY = the program's own entry, AT_BASE =
	 * ld.so's base. ld.so then loads the .so deps, relocates, and jumps to the program. */
	uintptr_t pc = prog.entry;	 /* what the seam jumps to (ld.so for a dynamic exec) */
	uintptr_t at_entry = prog.entry; /* AT_ENTRY = the program's own entry, always */
	uintptr_t at_base = 0;		 /* AT_BASE = ld.so base (0 when static) */
	prog.interp_loadmap = 0;
	int dynamic = prog.is_fdpic && prog.is_dynamic;
	if (dynamic) {
		const uint8_t *ld_data = NULL;
		size_t ld_len = 0;
		if (lxp_rootfs_resolve(g_cfg->rootfs, g_cfg->rootfs_count,
					   "/lib/ld-uClibc.so.0", &ld_data, &ld_len) != 0 ||
		    !ld_data)
			return -1; /* no interpreter in the rootfs */
		uintptr_t ld_base = (uintptr_t)region + ((prog.region_used + 15u) & ~15u);
		lxp_flat_t ld;
		int ldrc = lxp_loader_load_fdpic(&ld, ld_data, ld_len, (void *)ld_base,
						 LXP_PROG_REGION_SIZE -
							 (size_t)(ld_base - (uintptr_t)region),
						 1, 0); /* ld.so text is XIP from the rootfs (no copy) */
		if (ldrc != LXP_OK)
			return -1;
		pc = ld.entry;
		/* AT_BASE = ld.so's ELF header, which ld.so reads at _dl_start (dl-startup.c). With the
		 * text shared IN-PLACE, that header is in the cpio (ld.text_base), NOT at ld_base — which
		 * now holds only ld.so's RW block. (Pre-sharing, text+data were contiguous at ld_base, so
		 * the old `at_base = ld_base` happened to coincide with the header.) */
		at_base = ld.text_base;
		prog.interp_loadmap = ld.loadmap; /* r8 */
		/* r9 = ld.so's _DYNAMIC, NOT its GOT: uClibc-ng's FDPIC DL_BOOT_COMPUTE_DYN sets
		 * the dynamic-table ptr = dl_boot_ldso_dyn_pointer = the entry r9. (The working
		 * GOT is derived by __self_reloc.) Passing the GOT/base here mis-parses ld.so's
		 * dynamic → its RELATIVE relocs target wrong → _dl_malloc derefs an unrelocated
		 * GOT entry. */
		prog.got = ld.dynamic;
		prog.region_used = (size_t)(ld_base - (uintptr_t)region) + ld.region_used;
	}

	uint8_t *rw = region + ((prog.region_used + 15u) & ~15u);
	uint8_t *rw_end = region + LXP_PROG_REGION_SIZE;
	/* A dynamic proc's arena lives in the engine's PSRAM dyn_pool (ld.so mmaps libc.so
	 * ~500K from it); a static FDPIC proc uses the in-region 96K arena. The stack always sits
	 * in-region above the loaded image(s). */
	uint8_t *arena_mem = rw;
	size_t arena_sz = LXP_PROG_ARENA_SIZE;
	uint8_t *stack_lo = rw + LXP_PROG_ARENA_SIZE;
	if (dynamic) {
		if (!eng->dyn_pool)
			return -1; /* this engine has no room to host a dynamic proc */
		arena_mem = eng->dyn_pool(ridx, &arena_sz);
		stack_lo = rw; /* the region tail is the stack; the arena is in PSRAM */
	}
	lxp_arena_init(&g_arenas[ridx], arena_mem, arena_sz);
	lxp_proc_init(&g_lxp_proc[sidx], &g_arenas[ridx], 0x8000);
	g_lxp_proc[sidx].write_fn = g_cfg->write_fn;
	g_lxp_proc[sidx].read_fn = g_cfg->read_fn;
	g_lxp_proc[sidx].console_poll = g_cfg->console_poll;
	g_lxp_proc[sidx].io_ctx = g_cfg->io_ctx;
	g_lxp_proc[sidx].pid = pid;
	g_lxp_proc[sidx].ppid = ppid;
	/* Concurrent model: this slot is now a live process owning region ridx. */
	g_lxp_proc[sidx].alive = 1;
	g_lxp_proc[sidx].region = ridx;
	g_lxp_proc[sidx].region_owner = 1;
	/* access_ok bounds: this proc's own writable memory (image region + dynamic arena). The syscall
	 * layer rejects any user pointer outside these (+ the shared RO rootfs for reads). */
	g_lxp_proc[sidx].region_lo = (uintptr_t)region;
	g_lxp_proc[sidx].region_hi = (uintptr_t)region + LXP_PROG_REGION_SIZE;
	g_lxp_proc[sidx].pool_lo = (uintptr_t)arena_mem;
	g_lxp_proc[sidx].pool_hi = (uintptr_t)arena_mem + arena_sz;
	g_lxp_proc[sidx].is_fdpic = prog.is_fdpic;
	g_lxp_proc[sidx].is_dynamic = dynamic; /* arena/libc RW data lives in the dyn_pool */
	g_lxp_proc[sidx].stack_lo = (uintptr_t)stack_lo; /* writable-data / stack boundary (snapshot) */
	g_lxp_proc[sidx].snap_region = -1;		     /* no vfork snapshot outstanding */
	g_lxp_proc[sidx].umask = 022; /* standard default; a fork inherits it via the struct copy */
	g_lxp_proc[sidx].vfork_parent_slot = -1;
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
		if (cl >= sizeof(g_lxp_proc[sidx].comm))
			cl = sizeof(g_lxp_proc[sidx].comm) - 1;
		memcpy(g_lxp_proc[sidx].comm, base, cl);
		g_lxp_proc[sidx].comm[cl] = '\0';
	}
	lxp_proc_set_rootfs(&g_lxp_proc[sidx], g_cfg->rootfs, g_cfg->rootfs_count);
	void *sp = lxp_setup_stack(stack_lo, (size_t)(rw_end - stack_lo), argc, argv, NULL,
				       prog.is_fdpic, prog.phdr, prog.phnum, at_entry, at_base);
	if (!sp)
		return -1;
	/* Publish the program's runtime segment bases for the GDB source-level-debug helper. */
	g_lxp_dbg[sidx].text_base = prog.text_base;
	g_lxp_dbg[sidx].data_base = prog.data_base;
	g_lxp_dbg[sidx].entry = at_entry;
	g_lxp_dbg[sidx].dynamic = prog.dynamic; /* _DYNAMIC → DT_DEBUG → ld.so's link-map chain */
	g_lxp_dbg[sidx].interp_base = at_base;   /* ld.so text base (0 if static) */
	/* P3: a fresh image in this slot inherits no device mmap. Clear the dev_map ranges
	 * (they gate user_ok) and tear down any framebuffer region a prior occupant of this
	 * slot installed (map_device with size 0), so an exec/relaunch never leaks it. */
	g_lxp_proc[sidx].dev_map_lo[0] = g_lxp_proc[sidx].dev_map_hi[0] = 0;
	g_lxp_proc[sidx].dev_map_lo[1] = g_lxp_proc[sidx].dev_map_hi[1] = 0;
	if (eng->map_device)
		eng->map_device(sidx, 0, 0, 0);
	return eng->spawn_launch(sidx, ridx, &prog, (void *)pc, sp, stack_lo);
}

/* A child (cpid, status) exited: hand it to its parent (ppid). Wake a parent blocked
 * in wait4 (resume returning cpid + write *status), else queue the zombie for a later
 * wait4. Decrements the parent's live-children count either way. */
static void reap_to_parent(const struct lxp_engine *eng, int ppid, int cpid, int status)
{
	int pslot = -1;
	for (int t = 0; t < LXP_NSLOT; t++)
		if (g_lxp_proc[t].alive && g_lxp_proc[t].pid == ppid) {
			pslot = t;
			break;
		}
	if (pslot < 0)
		return;
	lxp_proc_t *par = &g_lxp_proc[pslot];
	if (par->live_children > 0)
		par->live_children--;
	if (par->wait_pending && (par->wait_pid <= 0 || par->wait_pid == cpid)) {
		if (par->wait_status_p) {
			/* Encode the wait status the way Linux does: a signal-killed child (our exit_status
			 * convention is 128 + signal) becomes WIFSIGNALED — low 7 bits = the signal — so the
			 * shell prints "Terminated"/"Killed", not "Done"; a normal exit stays WIFEXITED with
			 * the code in bits 8-15. (1..31 covers every signal we deliver.) */
			int raw = (status > 128 && status <= 128 + 31) ? (status - 128)
								       : ((status & 0xff) << 8);
			*(int *)(uintptr_t)par->wait_status_p = raw;
		}
		par->wait_pending = 0;
		if (g_lxp_used[pslot]) /* abort the parked-waiter spin thread first */
			eng->abort_slot(pslot);
		eng->spawn_resume(pslot, par->region, &g_ctx[pslot], cpid);
	} else {
		/* The parent is not blocking in wait4 (typically sitting in select()/poll() —
		 * busybox inetd's accept loop, dropbear's session relay). Queue the zombie for a
		 * later wait4 and raise SIGCHLD: the parent's handler runs, wait4()s the zombie, and
		 * closes the session / reaps the connection. Default action is IGNORE, so a parent
		 * without a handler is unaffected. Coalesce onto a free pending slot (as SIGALRM). */
		if (par->child_count < LXP_MAX_CHILD) {
			par->child_pid[par->child_count] = cpid;
			par->child_status[par->child_count] = status;
			par->child_count++;
		}
		if (!par->pending_sig)
			par->pending_sig = LXP_SIGCHLD;
	}
}

/* Deliver `sig` to a proc PARKED in rt_sigsuspend (the LinuxThreads restart). There is no live
 * frame — the interrupted context is the captured g_ctx[slot]. Save that as the slot's sigreturn
 * frame (to resume with `ret` = -EINTR), then resume the proc INTO its handler; the handler's
 * sa_restorer -> rt_sigreturn restores the saved frame and the syscall returns -EINTR. SIG_IGN
 * just resumes with `ret`; SIG_DFL terminates (the EV_EXIT pass reaps it). */
static void deliver_signal_parked(const struct lxp_engine *eng, int slot,
				  lxp_proc_t *proc, int sig, long ret)
{
	uintptr_t h = proc->sig_handler[sig];
	if (h == LXP_SIG_IGN || (h == LXP_SIG_DFL && sig == LXP_SIGCHLD)) {
		eng->spawn_resume(slot, proc->region, &g_ctx[slot], ret); /* IGN or SIGCHLD-default */
		return;
	}
	if (h == LXP_SIG_DFL) {
		proc->exited = 1;
		proc->exit_status = 128 + sig;
		return;
	}
	struct sig_save_s *sv = &g_sig_save[slot];
	sv->r0 = (uint32_t)ret;
	sv->r1 = sv->r2 = sv->r3 = 0; /* r0-r3 not captured; the sigsuspend caller reloads them */
	sv->r9 = g_ctx[slot].r4_11[5]; /* FDPIC GOT of the parked code — clobbered below (r4_11[5]=r9) */
	sv->r12 = g_ctx[slot].r12;
	sv->lr = g_ctx[slot].lr;
	sv->pc = g_ctx[slot].pc; /* the rt_sigsuspend resume point */
	sv->xpsr = (1u << 24);	 /* Thumb */
	sv->active = 1;
	/* Reuse the slot ctx as the handler-entry frame; sp + r4-r11 stay = the thread's, except r9
	 * (the handler's own GOT for FDPIC — resolve_handler derefs the {entry,GOT} funcdescs; the
	 * restart handler lives in libpthread, a different module than the interrupted libc). */
	uintptr_t entry, restorer;
	uint32_t got;
	resolve_handler(proc, sig, &entry, &got, &restorer);
	if (proc->is_fdpic)
		g_ctx[slot].r4_11[5] = got;	    /* r9 = handler's GOT */
	g_ctx[slot].lr = restorer | 1u;		    /* return -> sa_restorer entry -> sigreturn */
	g_ctx[slot].pc = entry | 1u;		    /* enter the handler (Thumb) */
	eng->spawn_resume(slot, proc->region, &g_ctx[slot], sig); /* r0 = signo */
}

/* ---- vfork data isolation (NOMMU) ------------------------------------------ */
/* A region may be reused as vfork snapshot scratch (which then becomes the child's exec image) or as
 * a fresh exec region ONLY if no live process occupies it. rowner[r] tracks the region's *owner*, but
 * that table is the sole liveness signal the pickers below consult, and under deep vfork nesting (a
 * pipeline over an SSH session: init+getty+inetd+dropbear+shell+members) accounting drift can leave a
 * live daemon's region reading rowner<0 — reusing it then memcpy's a foreign image straight over that
 * daemon's live libc data (the inetd flap: init's snapshot trampling inetd's __pthread thread block →
 * a garbage descriptor → p_errnop=0xffffffff → DACCVIOL). So gate every region pick on BOTH the owner
 * table AND actual process liveness. In the consistent state an owner's region always has rowner>=0,
 * so this is redundant and never refuses a genuinely-free region — it only refuses to trample a live
 * one (worst case a clean -ENOMEM fork refusal instead of a corruption), and it self-heals: once the
 * occupant exits, both the liveness scan and rowner agree the region is free again. */
static int region_free(int r, const int *rowner)
{
	if (rowner[r] >= 0)
		return 0;
	for (int s = 0; s < LXP_NSLOT; s++)
		if (g_lxp_proc[s].alive && g_lxp_proc[s].region == r)
			return 0; /* a live proc runs here despite rowner<0 — do not trample it */
	return 1;
}

/* NOMMU has no copy-on-write, so a vfork child SHARES the parent's region + dyn_pool. Correct vfork
 * usage restricts the child to exec/_exit, but real programs write shared data before exec (e.g.
 * dropbear's session child resets SIGCHLD to SIG_DFL, which uClibc-LinuxThreads records in a table in
 * the shared libc data) — corrupting the suspended parent. So we snapshot the parent's writable data
 * into a SPARE region at fork and restore it before the parent resumes. Storage is free: the spare
 * region's own region+dyn_pool exactly mirror the parent's (both LXP_PROG_*), and the child needs
 * that region for its eventual exec anyway. Only the writable image data [region, stack_lo) and the
 * dyn_pool are copied (the stack is skipped — the child uses frames below the shared sp, and the
 * parent's frames above are untouched); g_arenas[] allocator metadata is saved separately.
 * Returns the reserved scratch region index, or -1 if none is free (→ share, as before). */
static int vfork_snapshot(const struct lxp_engine *eng, lxp_proc_t *par, int child_slot,
			  int *rowner)
{
	int rsnap = -1;
	for (int r = 0; r < LXP_NREG; r++)
		if (region_free(r, rowner)) {
			rsnap = r;
			break;
		}
	if (rsnap < 0)
		return -1; /* no spare region: fall back to sharing (the pre-isolation behavior) */
	uint8_t *pr = eng->region(par->region);
	size_t dlen = par->stack_lo - (uintptr_t)pr; /* in-region writable data, below the stack */
	/* The coordinator reads guest SDRAM through its UNCACHED view; clean the parent's dirty cache
	 * lines to SDRAM first so the snapshot captures its live data (not stale SDRAM). */
	lxp_cache_clean(pr, dlen);
	memcpy(eng->region(rsnap), pr, dlen);
	if (par->is_dynamic && eng->dyn_pool) {
		size_t ds = 0;
		uint8_t *pdp = eng->dyn_pool(par->region, &ds);
		lxp_cache_clean(pdp, ds);
		memcpy(eng->dyn_pool(rsnap, NULL), pdp, ds);
	}
	g_snap_arena[child_slot] = g_arenas[par->region]; /* allocator metadata (coordinator memory) */
	rowner[rsnap] = child_slot;			  /* reserve it (also the child's exec region) */
	return rsnap;
}

/* Undo a vfork child's writes to the shared region before the parent resumes: copy the snapshot back
 * over the parent's region + dyn_pool and restore its arena metadata. */
static void vfork_restore(const struct lxp_engine *eng, lxp_proc_t *par, int rsnap,
			  int child_slot)
{
	uint8_t *pr = eng->region(par->region);
	size_t dlen = par->stack_lo - (uintptr_t)pr;
	/* Invalidate the parent's cache lines BEFORE the coordinator's uncached write, so the child's
	 * dirty lines are DISCARDED (not evicted) — otherwise an async eviction between the write and a
	 * post-write invalidate could clobber the fresh SDRAM restore (a non-deterministic corruption).
	 * The parent's own lines were already clean (vfork_snapshot flushed them), so only the child's
	 * writes are dropped. A second invalidate after the write drains the Device write buffer (DSB)
	 * and guarantees the parent refills from the restored SDRAM on resume. */
	lxp_cache_invalidate(pr, dlen);
	memcpy(pr, eng->region(rsnap), dlen);
	lxp_cache_invalidate(pr, dlen);
	if (par->is_dynamic && eng->dyn_pool) {
		size_t ds = 0;
		uint8_t *pdp = eng->dyn_pool(par->region, &ds);
		lxp_cache_invalidate(pdp, ds);
		memcpy(pdp, eng->dyn_pool(rsnap, NULL), ds);
		lxp_cache_invalidate(pdp, ds);
	}
	g_arenas[par->region] = g_snap_arena[child_slot];
}

int lxp_run_common(const struct lxp_engine *eng, const lxp_run_config_t *cfg,
		       const char *path, int argc, const char *const argv[])
{
	if (!eng || !cfg || !cfg->rootfs || !path || argc < 1 || !argv)
		return LXP_RUN_ELAUNCH;
	g_cfg = cfg;
	g_eng = eng;
	g_lxp_rootfs_lo = NULL; /* the cpio span — a seam's svc discrimination treats a cpio PC */
	g_lxp_rootfs_hi = NULL; /* as a program svc (the shared in-place text runs from here) */
	for (int i = 0; i < cfg->rootfs_count; i++) {
		const lxp_file_t *f = &cfg->rootfs[i];
		if (!f->data)
			continue;
		if (!g_lxp_rootfs_lo || f->data < g_lxp_rootfs_lo)
			g_lxp_rootfs_lo = f->data;
		if (!g_lxp_rootfs_hi || f->data + f->size > g_lxp_rootfs_hi)
			g_lxp_rootfs_hi = f->data + f->size;
	}
	for (int i = 0; i < LXP_NSLOT; i++) {
		g_lxp_used[i] = 0;
		g_lxp_proc[i].alive = 0;
	}
	g_pending_sig = 0;
	g_tty_isig = 1;
	lxp_stats_reset();
	for (int i = 0; i < LXP_NSLOT; i++)
		g_sig_save[i].active = 0;
#if defined(LXP_ENABLE_DEV)
	/* Register the Kconfig-enabled /dev class drivers (fb, input, ...) on this
	 * coordinator thread, where blocking HAL init (ove_fb_init, ove_i2c_create) is legal. */
	lxp_dev_autoreg_all();
#endif
#if defined(LXP_ENABLE_NETFS)
	/* Connect the remote-fs mount (9P Tversion/Tattach). Coordinator thread, where blocking
	 * init is legal; a down server is non-fatal — the mount reconnects lazily. */
	lxp_netfs_init();
#endif

	int bb = -1;
	for (int i = 0; i < cfg->rootfs_count; i++)
		if (strcmp(cfg->rootfs[i].path, path) == 0) {
			bb = i;
			break;
		}
	if (bb < 0 || !cfg->rootfs[bb].data)
		return LXP_RUN_ELAUNCH;

	/* Concurrent process model: the run loop COORDINATES the live process SET
	 * (g_lxp_proc[*].alive). Each live proc owns a region + an RTOS thread for
	 * its lifetime; a vfork parent resumes the instant its child execs into its own
	 * region (or exits) so the two co-run. rowner[r] = the slot owning region r. */
	int rowner[LXP_NREG]; /* slot that owns each region, or -1 */
	for (int r = 0; r < LXP_NREG; r++)
		rowner[r] = -1;

	g_lxp_active = 1;
	g_lxp_halt = 0;
	rowner[0] = 0;
	if (launch(eng, 0, 0, cfg->rootfs[bb].data, cfg->rootfs[bb].size, 1, 0, argc, argv, 0) != 0) {
		g_lxp_active = 0;
		return LXP_RUN_ELAUNCH;
	}
	g_lxp_proc[0].exec_file_idx = bb; /* the running image, for /proc/self/exe re-exec */

	int rc = LXP_RUN_ETIMEOUT;
	int next_pid = 2;
	int idle = 0;
	uint64_t last_refresh_us = 0;
	for (;;) {
		if (g_lxp_halt) { /* reboot(2)/poweroff: stop the whole system */
			rc = 0;
			break;
		}

		/* Claim ONE pending event under the crit (flags are atomic ints; the brief
		 * masked window keeps a preempting program svc from racing the read/clear).
		 * Act on it OUTSIDE the crit — abort/spawn/launch may yield. */
		int es = -1, et = 0;
		enum {
			EV_EXIT = 1,
			EV_EXEC,
			EV_FORK,
			EV_SLEEP,
			EV_WAITPARK,
			EV_PIPE,
			EV_DEVWAIT,
			EV_SOCKWAIT,
			EV_NETFSWAIT,
			EV_PTYWAIT,
			EV_SIGSUSPEND,
			EV_CONSOLEWAIT
		};
		eng->crit_enter();
		for (int s = 0; s < LXP_NSLOT; s++) {
			lxp_proc_t *p = &g_lxp_proc[s];
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
			if (p->wait_pending && g_lxp_used[s]) {
				es = s;
				et = EV_WAITPARK;
				break;
			}
			if (p->pipe_wait && g_lxp_used[s]) {
				es = s;
				et = EV_PIPE;
				break;
			}
			if (p->dev_wait && g_lxp_used[s]) {
				es = s;
				et = EV_DEVWAIT;
				break;
			}
			if (p->sock_wait && g_lxp_used[s]) {
				es = s;
				et = EV_SOCKWAIT;
				break;
			}
#if defined(LXP_ENABLE_NETFS)
			if (p->netfs_wait && g_lxp_used[s]) {
				es = s;
				et = EV_NETFSWAIT;
				break;
			}
#endif
#if defined(LXP_ENABLE_PTY)
			if (p->pty_wait && g_lxp_used[s]) {
				es = s;
				et = EV_PTYWAIT;
				break;
			}
#endif
			if (p->sigsuspend_pending && g_lxp_used[s]) {
				es = s;
				et = EV_SIGSUSPEND;
				break;
			}
			if (p->console_wait && g_lxp_used[s]) {
				es = s;
				et = EV_CONSOLEWAIT;
				break;
			}
		}
		eng->crit_exit();

		if (et ==
		    EV_FORK) { /* spawn a child sharing the parent's region; suspend the parent. */
			lxp_proc_t *par = &g_lxp_proc[es];
			int c = -1;
			for (int s = 0; s < LXP_NSLOT; s++)
				if (!g_lxp_proc[s].alive) {
					c = s;
					break;
				}
			if (c < 0) { /* no free slot: fail the fork (parent gets -ENOMEM). */
				eng->abort_slot(es);
				eng->spawn_resume(es, par->region, &g_ctx[es], -LXP_ENOMEM);
				idle = 0;
				continue;
			}
			lxp_proc_t *ch = &g_lxp_proc[c];
			*ch = *par; /* vfork shares the image + region */
#if defined(LXP_ENABLE_DEV)
			lxp_dev_fork_inherit(ch); /* the child shares the parent's device opens */
#endif
#if defined(LXP_ENABLE_NET)
			lxp_sock_fork_inherit(ch); /* the child shares the parent's socket opens */
#endif
#if defined(LXP_ENABLE_NETFS)
			lxp_netfs_fork_inherit(ch); /* the child shares the parent's remote-fs opens */
#endif
			ch->pid = next_pid++;
			ch->ppid = par->pid;
			ch->exited = ch->exec_pending = ch->fork_pending = 0;
			ch->sleep_pending = ch->wait_pending = ch->sleeping = 0;
			ch->pipe_wait = 0;
			ch->dev_wait = 0;
			ch->sock_wait = 0;
			ch->netfs_wait = 0;
			ch->netfs_req = -1;
			ch->sel_active = 0;
			ch->pty_wait = 0;
			ch->console_wait = 0;
			ch->pending_sig = 0;
			ch->alarm_deadline_us = 0; /* itimers are not inherited across fork */
			ch->alarm_interval_us = 0;
			ch->child_count = ch->live_children = 0;
			ch->clone_is_thread = 0;
			ch->snap_region = -1; /* set by vfork_snapshot below for a non-thread fork */
			ch->alive = 1;
			ch->region = par->region;
			ch->region_owner = 0; /* shares the parent's region */
			par->live_children++; /* the creator can waitpid()/join this child */
			if (par->clone_is_thread) {
				/* pthread: the child shares the region for LIFE and runs on its own
				 * stack; the parent CO-RUNS (gets the child tid). No vfork suspend —
				 * a thread never execs, so it co-runs with its creator immediately. */
				par->clone_is_thread = 0;
				ch->is_thread = 1;
				ch->vfork_parent_slot = -1;
				g_ctx[c] = g_ctx[es]; /* clone resumes from the parent's ctx... */
				g_ctx[c].sp =
					par->clone_child_stack; /* ...but on the child stack */
				eng->abort_slot(es);		/* drop the parent's parked task */
				eng->spawn_resume(es, par->region, &g_ctx[es],
						  ch->pid); /* parent co-runs, gets the tid */
				eng->spawn_resume(c, ch->region, &g_ctx[c],
						  0); /* child enters the thread fn (r0=0) */
				idle = 0;
				continue;
			}
			ch->is_thread = 0;
			ch->vfork_parent_slot =
				es; /* resume the parent when this child execs/exits */
			/* NOMMU vfork isolation: snapshot the parent's writable data so the child's
			 * pre-exec writes to the SHARED region can't corrupt the suspended parent. */
			ch->snap_region = vfork_snapshot(eng, par, c, rowner);
			if (ch->snap_region < 0) {
				/* No spare region to isolate the child's pre-exec writes from the
				 * suspended parent (deep vfork nesting — e.g. a pipeline over an SSH
				 * session: init+getty+inetd+dropbear+shell+members). Refuse the fork
				 * (-ENOMEM) rather than share the parent's region and let the child
				 * corrupt it; the caller sees a clean fork failure, not a fault. */
#if defined(LXP_ENABLE_DEV)
				lxp_dev_proc_exit(ch); /* undo the fd fork-inherit refcounts */
#endif
#if defined(LXP_ENABLE_NET)
				lxp_sock_proc_exit(ch);
#endif
#if defined(LXP_ENABLE_NETFS)
				lxp_netfs_proc_exit(ch);
#endif
				ch->alive = 0;
				g_lxp_used[c] = 0;
				if (par->live_children > 0)
					par->live_children--;
				eng->abort_slot(es);
				eng->spawn_resume(es, par->region, &g_ctx[es], -LXP_ENOMEM);
				idle = 0;
				continue;
			}
			eng->abort_slot(es); /* suspend the parent (no thread) */
			eng->spawn_resume(c, ch->region, &g_ctx[es],
					  0); /* child returns 0 from fork */
			idle = 0;
			continue;
		}

		if (et ==
		    EV_EXEC) { /* the child gets its own region → resume any vfork parent NOW. */
			lxp_proc_t *p = &g_lxp_proc[es];
			int idx = p->exec_file_idx, eargc = p->exec_argc;
			static char args[LXP_EXEC_ARGBUF];
			static const char *ptrs[LXP_EXEC_MAXARGS + 1];
			size_t off = 0;
			for (int j = 0; j < eargc; j++) {
				size_t n = strlen(p->exec_argv[j]) + 1;
				memcpy(args + off, p->exec_argv[j], n);
				ptrs[j] = args + off;
				off += n;
			}
			ptrs[eargc] = NULL;
			/* Reuse the reserved snapshot region as the exec image region — it's free once we
			 * restore the parent from it below, and reserving it at fork is why exec always has
			 * a region. No snapshot (region-pressure fallback) → find a free region as before. */
			int nr = p->snap_region;
			if (nr < 0)
				for (int r = 0; r < LXP_NREG; r++)
					if (region_free(r, rowner)) {
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
				g_lxp_used[es] = 0;
				if (vp >= 0)
					eng->spawn_resume(vp, g_lxp_proc[vp].region, &g_ctx[vp],
							  pid);
				reap_to_parent(eng, ppid, pid, 139);
				idle = 0;
				continue;
			}
			if (vp >=
			    0) { /* the child leaves the parent's region → the parent co-runs. */
				/* Undo the child's pre-exec writes to the shared region+dyn_pool BEFORE the
				 * parent runs again (the args were already copied out into `args` above, so
				 * restoring over them is safe). */
				if (p->snap_region >= 0)
					vfork_restore(eng, &g_lxp_proc[vp], p->snap_region, es);
				p->vfork_parent_slot = -1;
				eng->spawn_resume(vp, g_lxp_proc[vp].region, &g_ctx[vp], pid);
			}
			/* fds + cwd survive execve: preserve across the relaunch (launch re-inits). */
			lxp_fd_t saved_fds[LXP_MAX_FDS];
			char saved_cwd[LXP_PATH_MAX];
			memcpy(saved_fds, p->fds, sizeof(saved_fds));
			memcpy(saved_cwd, p->cwd, sizeof(saved_cwd));
			if (p->region_owner &&
			    rowner[p->region] == es) /* free the old owned region */
				rowner[p->region] = -1;
			rowner[nr] = es;
			eng->abort_slot(es);
			const uint8_t *img_data = cfg->rootfs[idx].data;
			size_t img_size = cfg->rootfs[idx].size;
			int rexec = 0;
#if defined(LXP_ENABLE_NETFS_EXEC)
			/* A program off the remote mount: its bytes are in the netfs exec staging
			 * buffer (RAM), not the rootfs; launch copies its text into the region (RWX). */
			if (idx == LXP_NETFS_EXEC_SENTINEL) {
				img_data = lxp_netfs_exec_image(&img_size);
				rexec = 1;
			}
#endif
			if (!img_data ||
			    launch(eng, es, nr, img_data, img_size, pid, ppid, eargc, ptrs, rexec) != 0) {
				rowner[nr] = -1;
				g_lxp_proc[es].alive = 0;
				g_lxp_used[es] = 0;
				reap_to_parent(eng, ppid, pid, 127);
				idle = 0;
				continue;
			}
			memcpy(g_lxp_proc[es].fds, saved_fds, sizeof(saved_fds));
			memcpy(g_lxp_proc[es].cwd, saved_cwd, sizeof(saved_cwd));
			g_lxp_proc[es].exec_file_idx = idx; /* remember the running image so a
								   later execv("/proc/self/exe") re-runs it */
			idle = 0;
			continue;
		}

		if (et ==
		    EV_EXIT) { /* reap: abort thread, free region, wake parent/queue zombie. */
			lxp_proc_t *p = &g_lxp_proc[es];
			int cpid = p->pid, status = p->exit_status, vp = p->vfork_parent_slot,
			    ppid = p->ppid;
#if defined(LXP_ENABLE_DEV)
			lxp_dev_proc_exit(p); /* release the exiting process's device opens */
#endif
#if defined(LXP_ENABLE_NET)
			lxp_sock_proc_exit(p); /* release the exiting process's socket opens */
#endif
#if defined(LXP_ENABLE_NETFS)
			lxp_netfs_proc_exit(p); /* release the exiting process's remote-fs opens */
#endif
			eng->abort_slot(es);
			if (p->region_owner && rowner[p->region] == es)
				rowner[p->region] = -1;
			p->alive = 0;
			g_lxp_used[es] = 0;
			if (es == 0) { /* init exited → the system is done */
				rc = status;
				break;
			}
			if (vp >= 0 && p->snap_region >= 0) {
				/* a vfork child died before exec (e.g. a failed exec) → undo its writes to
				 * the shared region so the parent is not corrupted, and free the scratch. */
				vfork_restore(eng, &g_lxp_proc[vp], p->snap_region, es);
				rowner[p->snap_region] = -1;
			}
			if (vp >=
			    0) /* fork-without-exec: the suspended parent resumes (vfork returns) */
				eng->spawn_resume(vp, g_lxp_proc[vp].region, &g_ctx[vp], cpid);
			reap_to_parent(eng, ppid, cpid, status);
			idle = 0;
			continue;
		}

		if (et ==
		    EV_SLEEP) { /* park the slot for the nanosleep duration (deadline below). */
			eng->abort_slot(es);
			g_lxp_proc[es].sleeping = 1;
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
		if (et == EV_DEVWAIT) { /* free the spin thread; the device retry below resumes it. */
			eng->abort_slot(es);
			idle = 0;
			continue;
		}
		if (et == EV_SOCKWAIT) { /* free the spin thread; the socket retry below resumes it. */
			eng->abort_slot(es);
			idle = 0;
			continue;
		}
#if defined(LXP_ENABLE_NETFS)
		if (et == EV_NETFSWAIT) { /* free the spin thread; the netfs retry below resumes it. */
			eng->abort_slot(es);
			idle = 0;
			continue;
		}
#endif
#if defined(LXP_ENABLE_PTY)
		if (et == EV_PTYWAIT) { /* free the spin thread; the pty retry below resumes it. */
			eng->abort_slot(es);
			idle = 0;
			continue;
		}
#endif
		if (et == EV_SIGSUSPEND) { /* free the spin thread; pending_sig below wakes it. */
			eng->abort_slot(es);
			idle = 0;
			continue;
		}
		if (et == EV_CONSOLEWAIT) { /* free the spin thread; the console retry below resumes it. */
			eng->abort_slot(es);
			idle = 0;
			continue;
		}

		/* No pending event: resume any sleeper whose deadline passed; assess liveness. */
		uint64_t now = 0;
		lxp_time_us(&now);
		int progress = 0, any_alive = 0, any_busy = 0, any_pipe_wait = 0, any_dev_wait = 0,
		    any_sock_wait = 0, any_netfs_wait = 0, any_pty_wait = 0, any_console_wait = 0;
		uint64_t next_sleep = UINT64_MAX; /* earliest future sleeper deadline (µs) */
		for (int s = 0; s < LXP_NSLOT; s++) {
			lxp_proc_t *p = &g_lxp_proc[s];
			if (!p->alive)
				continue;
			any_alive = 1;
			if (g_lxp_used[s])
				any_busy = 1;
			if (p->pipe_wait)
				any_pipe_wait = 1;
			if (p->dev_wait)
				any_dev_wait = 1;
			if (p->sock_wait)
				any_sock_wait = 1;
			if (p->netfs_wait)
				any_netfs_wait = 1;
			if (p->pty_wait)
				any_pty_wait = 1;
			if (p->console_wait)
				any_console_wait = 1;
			/* ITIMER_REAL: raise SIGALRM once the deadline passes (setitimer/alarm).
			 * Only for a parked proc — a running one owns these fields and takes the
			 * signal at its next syscall boundary. Re-arm from the interval, and clamp
			 * the coordinator's idle sleep to the (possibly new) deadline. */
			if (p->alarm_deadline_us && !g_lxp_used[s]) {
				if (now >= p->alarm_deadline_us) {
					if (!p->pending_sig)
						p->pending_sig = LXP_SIGALRM;
					p->alarm_deadline_us =
						p->alarm_interval_us ? now + p->alarm_interval_us : 0;
				}
				if (p->alarm_deadline_us && p->alarm_deadline_us < next_sleep)
					next_sleep = p->alarm_deadline_us;
			}
			/* Cross-process signal (D3) to a parked, blocked proc: the dispatch can't
			 * deliver (no live thread), so the coordinator does. SIG_IGN is dropped;
			 * otherwise terminate (default action — a custom handler on a blocked proc
			 * is approximated as terminate). EV_EXIT reaps it next pass. */
			if (p->pending_sig && !g_lxp_used[s] && p->sigsuspend_pending) {
				/* rt_sigsuspend-parked thread woken by a delivered signal (the
				 * LinuxThreads restart): run the handler, then resume the syscall
				 * returning -EINTR. Unlike a sleep/wait/pipe block (terminated by a
				 * default-action signal), sigsuspend EXPECTS the signal and continues. */
				int sig = p->pending_sig;
				p->pending_sig = 0;
				p->sigsuspend_pending = 0;
				deliver_signal_parked(eng, s, p, sig, -LXP_EINTR);
				progress = 1;
			} else if (p->pending_sig && !g_lxp_used[s] &&
				   (p->sleeping || p->wait_pending || p->pipe_wait)) {
				int sig = p->pending_sig;
				p->pending_sig = 0;
				if (sig == LXP_SIGCHLD) {
					/* A child of a proc blocked in sleep/wait/pipe exited. SIGCHLD
					 * never terminates: run a handler (the op then returns -EINTR),
					 * else swallow it and stay parked (default action = ignore). */
					if (!sig_swallowed(p, sig)) {
						p->sleeping = p->wait_pending = p->pipe_wait = 0;
						deliver_signal_parked(eng, s, p, sig, -LXP_EINTR);
						progress = 1;
					}
				} else if (p->sig_handler[sig] != LXP_SIG_IGN) {
					p->exited = 1;
					p->exit_status = 128 + sig;
					p->sleeping = p->wait_pending = p->pipe_wait = 0;
					progress = 1;
				}
			} else if (p->pending_sig && !g_lxp_used[s] && p->sock_wait) {
				/* Signal to a proc parked in a socket op (connect/recv/accept/
				 * poll): run a handler then the op returns -EINTR — busybox ping's
				 * SIGALRM timer drives its next send this way, and a server's
				 * blocked accept takes SIGTERM/SIGCHLD. SIG_DFL terminates; SIG_IGN
				 * (and SIGCHLD's default-ignore) is swallowed, leaving the proc
				 * parked (the retry re-attempts). */
				int sig = p->pending_sig;
				p->pending_sig = 0;
				if (!sig_swallowed(p, sig)) {
					p->sock_wait = 0;
					deliver_signal_parked(eng, s, p, sig, -LXP_EINTR);
					progress = 1;
				}
#if defined(LXP_ENABLE_PTY)
			} else if (p->pending_sig && !g_lxp_used[s] && p->pty_wait) {
				/* Signal to a proc parked in a pty read/write: ^C (SIGINT) from the line
				 * discipline lands here — the shell's handler runs and its slave read
				 * returns -EINTR (re-prompt); a foreground child takes it by default
				 * action. SIG_IGN (and SIGCHLD's default-ignore) leaves it parked. */
				int sig = p->pending_sig;
				p->pending_sig = 0;
				if (!sig_swallowed(p, sig)) {
					p->pty_wait = 0;
					deliver_signal_parked(eng, s, p, sig, -LXP_EINTR);
					progress = 1;
				}
			}
#else
			}
#endif
			if (p->sleeping) {
				any_busy = 1;
				if (now >= p->sleep_until_us) {
					p->sleeping = 0;
					eng->spawn_resume(s, p->region, &g_ctx[s], 0);
					progress = 1;
				} else if (p->sleep_until_us < next_sleep) {
					next_sleep = p->sleep_until_us; /* wake the coordinator at this deadline */
				}
			}
			/* Blocked pipe I/O: retry now that a peer may have drained/filled the
			 * ring (or closed its end → EOF/EPIPE); resume the proc when it completes. */
			if (p->pipe_wait && !g_lxp_used[s]) {
				long r = lxp_pipe_retry(p);
				if (r != -LXP_EAGAIN) {
					p->pipe_wait = 0;
					if (r == -LXP_EPIPE &&
					    p->sig_handler[LXP_SIGPIPE] != LXP_SIG_IGN) {
						/* broken pipe + default SIGPIPE → terminate the
						 * writer; the EV_EXIT pass reaps it (no live thread). */
						p->exited = 1;
						p->exit_status = 128 + LXP_SIGPIPE;
					} else {
						eng->spawn_resume(s, p->region, &g_ctx[s], r);
					}
					progress = 1;
				}
			}
#if defined(LXP_ENABLE_DEV)
			/* Blocked device I/O: retry (the driver may now have data/space); resume
			 * the proc when the op completes. Mirrors the pipe retry above. */
			if (p->dev_wait == LXP_DEVW_MMAP && !g_lxp_used[s]) {
				/* Device mmap (P3): install the unprivileged MPU region over the
				 * device buffer on THIS (coordinator) thread — domain/TCB edits are
				 * not svc-exception-safe — record it for user_ok, and resume the proc
				 * with r0 = the mapped address (or -errno if the engine can't map). */
				long r = eng->map_device
						 ? (eng->map_device(s, (uintptr_t)p->dev_buf, p->dev_len,
								    (unsigned)p->dev_cmd) == 0
							    ? (long)p->dev_buf
							    : -LXP_ENOMEM)
						 : -LXP_ENODEV;
				if (r >= 0) {
					p->dev_map_lo[0] = (uintptr_t)p->dev_buf;
					p->dev_map_hi[0] = (uintptr_t)p->dev_buf + p->dev_len;
				}
				p->dev_wait = 0;
				eng->spawn_resume(s, p->region, &g_ctx[s], r);
				progress = 1;
			} else if (p->dev_wait && !g_lxp_used[s]) {
				long r = lxp_dev_retry(p);
				if (r != -LXP_EAGAIN) {
					p->dev_wait = 0;
					eng->spawn_resume(s, p->region, &g_ctx[s], r);
					progress = 1;
				}
			}
#endif
#if defined(LXP_ENABLE_NET)
			/* Blocked socket I/O: retry (connect may have completed, or data/space
			 * arrived); resume the proc when the op completes. Mirrors the pipe/device
			 * retries above. */
			if (p->sock_wait && !g_lxp_used[s]) {
				long r = lxp_sock_retry(p);
				if (r != -LXP_EAGAIN) {
					p->sock_wait = 0;
					eng->spawn_resume(s, p->region, &g_ctx[s], r);
					progress = 1;
				}
			}
#endif
#if defined(LXP_ENABLE_NETFS)
			/* Blocked remote-fs I/O: pump the 9P transport + advance the parked op;
			 * resume the proc when its reply completes. Mirrors the socket retry. */
			if (p->netfs_wait && !g_lxp_used[s]) {
				long r = lxp_netfs_retry(p);
				if (r != -LXP_EAGAIN) {
					p->netfs_wait = 0;
					/* a completed exec-fetch sets exec_pending → EV_EXEC launches it
					 * from the staging buffer; don't resume the parked execve. */
					if (!p->exec_pending)
						eng->spawn_resume(s, p->region, &g_ctx[s], r);
					progress = 1;
				}
			}
#endif
#if defined(LXP_ENABLE_PTY)
			/* Blocked pty I/O: retry (the peer end may have drained/filled the ring);
			 * resume the proc when the op completes. Mirrors the pipe/socket retries. */
			if (p->pty_wait && !g_lxp_used[s]) {
				long r = lxp_pty_retry(p);
				if (r != -LXP_EAGAIN) {
					p->pty_wait = 0;
					eng->spawn_resume(s, p->region, &g_ctx[s], r);
					progress = 1;
				}
			}
#endif
			/* Blocked console read: the coordinator polls readiness (so read_fn does
			 * not busy-wait in the SVC handler and starve background tasks like the net
			 * RX poll). When a key is ready, read it here and resume the proc. */
			if (p->console_wait && !g_lxp_used[s]) {
				if (p->console_poll && p->console_poll(p->io_ctx)) {
					long r = p->read_fn ? p->read_fn(p->io_ctx, 0,
									 (void *)p->console_buf,
									 p->console_len)
							    : 0;
					p->console_wait = 0;
					eng->spawn_resume(s, p->region, &g_ctx[s], r);
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
			rc = LXP_RUN_ETIMEOUT;
			break;
		}
		if (now - last_refresh_us >= 200000ull) {
			last_refresh_us = now;
			refresh_stats();
		}
#if defined(LXP_ENABLE_DEV)
		lxp_dev_tick(now); /* coordinator-thread periodic work (fb flush, touch poll) */
#endif
#if defined(LXP_ENABLE_NETFS)
		lxp_netfs_tick(now); /* pump the 9P transport: background clunks + lazy reconnect */
#endif
		/* Block until a program parks (event_post) or the timeout. NOT a busy 1ms poll —
		 * that would preempt running programs every tick and reset their time-slice,
		 * starving a fg command while a CPU-bound background job runs. The timeout is the
		 * NEAREST sleeper deadline (so nanosleep wakes on time, not quantized to the old
		 * fixed 50ms), clamped to a short poll while a pipe is blocked; a pipe read/write
		 * that unblocks a peer also kicks us directly (lxp_pipe_kick). */
		unsigned to = (any_pipe_wait || any_dev_wait || any_sock_wait || any_netfs_wait ||
			       any_pty_wait || any_console_wait)
				      ? 5u
				      : 50u;
		if (next_sleep != UINT64_MAX && next_sleep > now) {
			uint64_t d_ms = (next_sleep - now + 999u) / 1000u; /* round up, don't wake early */
			if (d_ms < 1u)
				d_ms = 1u;
			if (d_ms < (uint64_t)to)
				to = (unsigned)d_ms;
		}
		eng->event_wait(to);
	}
	/* Tear down any still-running slot tasks so a subsequent lxp_run() starts
	 * clean and no leaked task starves the next program. */
	for (int i = 0; i < LXP_NSLOT; i++)
		if (g_lxp_used[i])
			eng->abort_slot(i);
	g_lxp_active = 0;
	return rc;
}
