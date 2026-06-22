/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr engine seam for the Linux personality (see include/ove/linux/zephyr.h).
 * Binds the engine-agnostic syscall core to a Zephyr/Cortex-M target: traps the
 * unprivileged program's svc, runs each loaded bFLT in its own MPU domain, and
 * drives the NOMMU process model.
 *
 * NOMMU process model (sequentialised, observationally identical to vfork for
 * the shell pattern since the parent waitpid()s anyway):
 *  - vfork: the seam captures the parent's full resume context (r4-r11, r12, lr,
 *    sp, pc at the svc return) and parks it; the run loop spawns a CHILD thread
 *    that resumes at that context with r0=0, sharing the parent's region until
 *    it execs.
 *  - execve: the run loop loads the new image into a SECOND region/domain.
 *  - child exit: the run loop queues the status on the parent for wait4, then
 *    resumes the parent at the captured context with r0=child_pid.
 *
 * Each program runs in its own k_mem_domain (program partitions + libc/heap) so
 * the privileged run loop (default domain) can always (re)load a region.
 */

#include <zephyr/kernel.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <string.h>

#include "ove/arena.h"
#include "ove/loader.h"
#include "ove/linux/syscall.h"
#include "ove/linux/zephyr.h"

/* ---- program memory: two regions, so a parent + child image coexist -------- */
#define PROG_REGION_SIZE 0x60000u /* 384K: BusyBox loads ~129K + arena + stack */
#define PROG_ARENA_SIZE 0x18000u  /* 96K heap for the program */
#define NREG 2
#define NSLOT 2

K_APPMEM_PARTITION_DEFINE(ove_lnx_prog_partition);
K_APP_BMEM(ove_lnx_prog_partition)
static uint8_t prog_regions[NREG][PROG_REGION_SIZE] __aligned(32);
static ove_arena_t g_arenas[NREG];

/* A user-readable partition (in every program domain) for the vfork resume
 * context, which the unprivileged resume thread reads to replay its registers.
 * Kernel .bss would be privileged-only and fault under the K_USER thread. */
K_APPMEM_PARTITION_DEFINE(ove_lnx_shared_partition);

/* Per-region MPU domain: program text/data + the libc/heap partitions. */
extern struct k_mem_partition z_libc_partition;
extern struct k_mem_partition z_malloc_partition;
static struct k_mem_domain g_domains[NREG];
static struct k_mem_partition g_text[NREG], g_data[NREG];
static int g_dom_inited[NREG];

/* ---- per-process slots ----------------------------------------------------- */
struct slot {
	ove_lnx_proc_t proc;
	struct k_thread thread;
	k_tid_t tid;
	int used;
};
static struct slot g_slots[NSLOT];
K_THREAD_STACK_ARRAY_DEFINE(g_tramp_stacks, NSLOT, 1024);

static const ove_lnx_zephyr_config_t *g_cfg;
static volatile int g_lnx_active;

/* Captured parent context at a vfork svc, replayed to resume parent + child. */
struct resume_ctx {
	uint32_t r4_11[8];
	uint32_t r12;
	uint32_t lr;
	uint32_t sp;
	uint32_t pc;
};
K_APP_BMEM(ove_lnx_shared_partition) static struct resume_ctx g_vfork_ctx;
static volatile int g_vfork_pending;

/* Saved interrupted context for an in-flight signal handler. Only the privileged
 * seam reads/writes it (no user trampoline), so it stays in kernel .bss. r4-r11
 * are NOT saved: a C handler preserves them, so they are already correct when
 * rt_sigreturn runs. No nesting (one handler at a time). */
static struct {
	uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
	int active;
} g_sig_save;

/* Terminal line discipline + async-signal latch (the host's read_fn drives both
 * via ove_lnx_zephyr_tty_isig()/ove_lnx_zephyr_post_signal()). */
static volatile int g_tty_isig = 1;
static volatile int g_pending_sig;

int ove_lnx_zephyr_tty_isig(void)
{
	return g_tty_isig;
}

void ove_lnx_zephyr_post_signal(int sig)
{
	g_pending_sig = sig;
}

/* Where a program parks (in its own text) after a blocking syscall, until the
 * run loop reaps/relaunches/resumes it. */
static void park_loop(void)
{
	for (;;) {
	}
}

static struct slot *current_slot(void)
{
	k_tid_t t = k_current_get();
	for (int i = 0; i < NSLOT; i++)
		if (g_slots[i].used && g_slots[i].tid == t)
			return &g_slots[i];
	return NULL;
}

/* Deliver signal `sig` to slot `s`.
 * Real handler -> save the post-syscall context, redirect to the handler with
 *                 r0=sig and lr=libc sa_restorer (which calls rt_sigreturn).
 * SIG_IGN      -> the syscall simply returns `ret`.
 * SIG_DFL      -> default action: terminate (status 128+sig), park for reaping.
 * `ret` is the value the interrupted syscall returns once the handler is done
 * (0 for a kill/tkill, -EINTR for a console-interrupted read). */
static void deliver_signal(struct arch_esf *esf, _callee_saved_t *callee, struct slot *s, int sig,
			   long ret)
{
	ARG_UNUSED(callee);
	if (sig < 1 || sig >= OVE_LNX_NSIG) {
		esf->basic.r0 = (uint32_t)-OVE_LNX_EINVAL;
		return;
	}
	uintptr_t h = s->proc.sig_handler[sig];
	if (h == OVE_LNX_SIG_IGN) {
		esf->basic.r0 = (uint32_t)ret;
		return;
	}
	if (h == OVE_LNX_SIG_DFL) {
		s->proc.exited = 1;
		s->proc.exit_status = 128 + sig;
		esf->basic.pc = ((uint32_t)&park_loop) | 1u;
		return;
	}
	g_sig_save.r0 = (uint32_t)ret; /* the interrupted syscall's return value */
	g_sig_save.r1 = esf->basic.r1;
	g_sig_save.r2 = esf->basic.r2;
	g_sig_save.r3 = esf->basic.r3;
	g_sig_save.r12 = esf->basic.ip;
	g_sig_save.lr = esf->basic.lr;
	g_sig_save.pc = esf->basic.pc;
	g_sig_save.xpsr = esf->basic.xpsr;
	g_sig_save.active = 1;
	esf->basic.pc = h | 1u;
	esf->basic.r0 = (uint32_t)sig;
	esf->basic.lr = s->proc.sig_restorer[sig] | 1u;
}

/* rt_sigreturn: restore the context saved at delivery (r4-r11 were preserved by
 * the C handler, so they need no restore). */
static void sig_restore(struct arch_esf *esf)
{
	if (!g_sig_save.active)
		return;
	esf->basic.r0 = g_sig_save.r0;
	esf->basic.r1 = g_sig_save.r1;
	esf->basic.r2 = g_sig_save.r2;
	esf->basic.r3 = g_sig_save.r3;
	esf->basic.ip = g_sig_save.r12;
	esf->basic.lr = g_sig_save.lr;
	esf->basic.pc = g_sig_save.pc;
	esf->basic.xpsr = g_sig_save.xpsr;
	g_sig_save.active = 0;
}

/* ---- the Linux SVC seam ---------------------------------------------------- */
extern void __real_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
				    uint32_t exc_return);

void __wrap_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
			     uint32_t exc_return)
{
	if (g_lnx_active) {
		const uint16_t *svc = (const uint16_t *)(esf->basic.pc - 2);
		if ((*svc & 0xff00u) == 0xdf00u && (*svc & 0x00ffu) == 0x00u) {
			struct slot *s = current_slot();
			if (s) {
				long nr = (long)callee->v4; /* r7 */
				/* Track the tty ISIG mode so console ^C knows whether to raise
				 * SIGINT (canonical) or pass ^C through (the shell's raw editor). */
				if (nr == OVE_LNX_NR_ioctl) {
					unsigned long cmd = (unsigned long)esf->basic.r1;
					if (cmd == OVE_LNX_TCSETS || cmd == OVE_LNX_TCSETSW ||
					    cmd == OVE_LNX_TCSETSF) {
						const ove_lnx_termios *t =
							(const ove_lnx_termios *)(uintptr_t)
								esf->basic.r2;
						if (t)
							g_tty_isig =
								(t->c_lflag & OVE_LNX_ISIG) ? 1 : 0;
					}
				}
				/* Signal delivery (kill/tkill/tgkill) + return need the trap
				 * frame, so the seam handles them directly. */
				if (nr == OVE_LNX_NR_kill || nr == OVE_LNX_NR_tkill ||
				    nr == OVE_LNX_NR_tgkill) {
					int sig = (nr == OVE_LNX_NR_tgkill) ? (int)esf->basic.r2
									    : (int)esf->basic.r1;
					deliver_signal((struct arch_esf *)esf, callee, s, sig, 0);
					return;
				}
				if (nr == OVE_LNX_NR_rt_sigreturn || nr == OVE_LNX_NR_sigreturn) {
					sig_restore((struct arch_esf *)esf);
					return;
				}
				if (nr == OVE_LNX_NR_vfork || nr == OVE_LNX_NR_fork) {
					/* Capture the resume context; the parent's
					 * vfork return (0xd8) does mov r7,ip; bxcc lr,
					 * so r12 + lr matter as well as r4-r11/sp/pc. */
					g_vfork_ctx.r4_11[0] = callee->v1;
					g_vfork_ctx.r4_11[1] = callee->v2;
					g_vfork_ctx.r4_11[2] = callee->v3;
					g_vfork_ctx.r4_11[3] = callee->v4;
					g_vfork_ctx.r4_11[4] = callee->v5;
					g_vfork_ctx.r4_11[5] = callee->v6;
					g_vfork_ctx.r4_11[6] = callee->v7;
					g_vfork_ctx.r4_11[7] = callee->v8;
					g_vfork_ctx.r12 = esf->basic.ip;
					g_vfork_ctx.lr = esf->basic.lr;
					/* callee->psp points at the hardware-stacked
					 * exception frame (r0..xpsr, 8 words); the
					 * program's pre-svc SP is 32 bytes above it
					 * (+4 if the stacker inserted 8-byte-align pad). */
					g_vfork_ctx.sp = callee->psp + 32u +
							 ((esf->basic.xpsr & (1u << 9)) ? 4u : 0u);
					g_vfork_ctx.pc = esf->basic.pc | 1u; /* Thumb bit for bx */
					g_vfork_pending = 1;
					((struct arch_esf *)esf)->basic.pc =
						((uint32_t)&park_loop) | 1u;
					return;
				}
				long r = ove_lnx_syscall(&s->proc, nr, (int32_t)esf->basic.r0,
							 (int32_t)esf->basic.r1,
							 (int32_t)esf->basic.r2,
							 (int32_t)esf->basic.r3,
							 (int32_t)callee->v1, (int32_t)callee->v2);
				if (r == -OVE_LNX_ENOSYS && g_cfg && g_cfg->on_enosys)
					g_cfg->on_enosys(nr);
				if (s->proc.exited || s->proc.exec_pending) {
					((struct arch_esf *)esf)->basic.pc =
						((uint32_t)&park_loop) | 1u;
					return;
				}
				/* A console ^C latched a signal during this syscall (e.g. a
				 * read): deliver it now, resuming the syscall with its result
				 * (-EINTR), the Linux at-the-boundary async-delivery model. */
				if (g_pending_sig) {
					int sig = g_pending_sig;
					g_pending_sig = 0;
					deliver_signal((struct arch_esf *)esf, callee, s, sig, r);
					return;
				}
				((struct arch_esf *)esf)->basic.r0 = (uint32_t)r;
				return;
			}
		}
	}
	__real_z_do_kernel_oops(esf, callee, exc_return);
}

/* Enter a freshly loaded program: SP -> argc block, r0 = 0 (static fini). */
static void arg_tramp(void *sp, void *entry, void *unused)
{
	ARG_UNUSED(unused);
	__asm__ volatile("mov sp, %0\n mov r0, #0\n bx %1\n"
			 :
			 : "r"(sp), "r"(entry)
			 : "r0", "memory");
	__builtin_unreachable();
}

/* Resume a parked program at a captured context with a chosen r0 (vfork return). */
static void resume_tramp(void *r0val, void *ctx, void *unused)
{
	ARG_UNUSED(unused);
	register void *rv __asm__("r0") = r0val;
	register void *c __asm__("r1") = ctx;
	__asm__ volatile("ldmia r1!, {r4-r11}\n" /* r4-r11 */
			 "ldr r12, [r1], #4\n"	 /* r12 */
			 "ldr lr, [r1], #4\n"	 /* lr */
			 "ldr sp, [r1], #4\n"	 /* sp */
			 "ldr r1, [r1]\n"	 /* pc -> r1 */
			 "bx r1\n"
			 :
			 : "r"(rv), "r"(c)
			 : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "lr", "memory");
	__builtin_unreachable();
}

/* (Re)build region ridx's MPU domain (W^X text/data split) for a loaded image. */
static int setup_domain(int ridx, const ove_flat_t *prog)
{
	uint8_t *region = prog_regions[ridx];
	if (g_dom_inited[ridx]) {
		k_mem_domain_remove_partition(&g_domains[ridx], &g_text[ridx]);
		k_mem_domain_remove_partition(&g_domains[ridx], &g_data[ridx]);
	}
	g_text[ridx].start = (uintptr_t)region;
	g_text[ridx].size = prog->text_size;
	g_text[ridx].attr = K_MEM_PARTITION_P_RX_U_RX;
	g_data[ridx].start = (uintptr_t)region + prog->text_size;
	g_data[ridx].size = PROG_REGION_SIZE - prog->text_size;
	g_data[ridx].attr = K_MEM_PARTITION_P_RW_U_RW;
	if (!g_dom_inited[ridx]) {
		struct k_mem_partition *base[] = {&z_libc_partition, &z_malloc_partition,
						  &ove_lnx_shared_partition};
		if (k_mem_domain_init(&g_domains[ridx], 3, base) != 0)
			return -1;
		g_dom_inited[ridx] = 1;
	}
	if (k_mem_domain_add_partition(&g_domains[ridx], &g_text[ridx]) != 0 ||
	    k_mem_domain_add_partition(&g_domains[ridx], &g_data[ridx]) != 0)
		return -1;
	return 0;
}

/* Load a bFLT into region ridx and run it as slot sidx's user thread. */
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
	void *sp = ove_lnx_setup_stack(stack_lo, (size_t)(rw_end - stack_lo), argc, argv, NULL);
	if (!sp || setup_domain(ridx, &prog) != 0)
		return -1;
	g_slots[sidx].tid = k_thread_create(&g_slots[sidx].thread, g_tramp_stacks[sidx],
					    K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]), arg_tramp,
					    sp, (void *)prog.entry, NULL, 5, K_USER, K_FOREVER);
	g_slots[sidx].used = 1;
	k_mem_domain_add_thread(&g_domains[ridx], g_slots[sidx].tid);
	k_thread_start(g_slots[sidx].tid);
	return 0;
}

/* Resume slot sidx (in region ridx's domain) at the captured vfork context. */
static void resume_slot(int sidx, int ridx, long r0val)
{
	g_slots[sidx].tid = k_thread_create(&g_slots[sidx].thread, g_tramp_stacks[sidx],
					    K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]),
					    resume_tramp, (void *)r0val, &g_vfork_ctx, NULL, 5,
					    K_USER, K_FOREVER);
	g_slots[sidx].used = 1;
	k_mem_domain_add_thread(&g_domains[ridx], g_slots[sidx].tid);
	k_thread_start(g_slots[sidx].tid);
}

int ove_lnx_zephyr_run(const ove_lnx_zephyr_config_t *cfg, const char *path, int argc,
		       const char *const argv[])
{
	if (!cfg || !cfg->rootfs || !path || argc < 1 || !argv)
		return OVE_LNX_ZEPHYR_ELAUNCH;
	g_cfg = cfg;
	for (int i = 0; i < NSLOT; i++)
		g_slots[i].used = 0;
	g_vfork_pending = 0;
	g_pending_sig = 0;
	g_tty_isig = 1;
	g_sig_save.active = 0;

	/* Resolve the init program in the rootfs (must be a regular file). */
	int bb = -1;
	for (int i = 0; i < cfg->rootfs_count; i++)
		if (strcmp(cfg->rootfs[i].path, path) == 0) {
			bb = i;
			break;
		}
	if (bb < 0 || !cfg->rootfs[bb].data)
		return OVE_LNX_ZEPHYR_ELAUNCH;

	g_lnx_active = 1;
	if (launch_slot(0, 0, cfg->rootfs[bb].data, cfg->rootfs[bb].size, 1, 0, argc, argv) != 0) {
		g_lnx_active = 0;
		return OVE_LNX_ZEPHYR_ELAUNCH;
	}

	int rc = OVE_LNX_ZEPHYR_ETIMEOUT;
	int next_pid = 2;  /* assigned to each new child */
	int cur_child = 2; /* the child currently being spawned/reaped */
	for (int i = 0; i < 8000; i++) {
		/* The program vfork()ed: spawn the child resuming at vfork (r0=0),
		 * sharing the parent's region/domain until it execs; park the parent
		 * (abort its thread — it is replayed after the child exits). */
		if (g_vfork_pending) {
			g_vfork_pending = 0;
			cur_child = next_pid++;
			g_slots[1].proc = g_slots[0].proc; /* same arena/fds/rootfs */
			g_slots[1].proc.pid = cur_child;
			g_slots[1].proc.ppid = 1;
			g_slots[1].proc.exited = 0;
			g_slots[1].proc.exec_pending = 0;
			g_slots[1].proc.child_count = 0; /* the child inherits no zombies */
			k_thread_abort(g_slots[0].tid);
			g_slots[0].used = 0; /* parent parked; resumed after child exit */
			resume_slot(1, 0, 0);
			continue;
		}
		/* The child execve()d: load the new image into region 1 + run it. */
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
			/* fds survive execve (no close-on-exec here): preserve the table so
			 * a pipe wired by dup2 before exec reaches the new image. */
			ove_lnx_fd_t saved_fds[OVE_LNX_MAX_FDS];
			memcpy(saved_fds, g_slots[1].proc.fds, sizeof(saved_fds));
			k_thread_abort(g_slots[1].tid);
			if (launch_slot(1, 1, cfg->rootfs[idx].data, cfg->rootfs[idx].size,
					cur_child, 1, eargc, ptrs) != 0) {
				rc = OVE_LNX_ZEPHYR_EEXEC;
				break;
			}
			memcpy(g_slots[1].proc.fds, saved_fds, sizeof(saved_fds));
			continue;
		}
		/* The child exited: record its status for wait4, then resume the parent
		 * at the vfork context with r0 = child_pid. */
		if (g_slots[1].used && g_slots[1].proc.exited) {
			int status = g_slots[1].proc.exit_status;
			k_thread_abort(g_slots[1].tid);
			g_slots[1].used = 0;
			ove_lnx_proc_t *par = &g_slots[0].proc;
			if (par->child_count < OVE_LNX_MAX_CHILD) {
				par->child_pid[par->child_count] = cur_child;
				par->child_status[par->child_count] = status;
				par->child_count++;
			}
			resume_slot(0, 0, cur_child);
			continue;
		}
		/* init (slot 0) exited: done. */
		if (g_slots[0].used && g_slots[0].proc.exited) {
			rc = g_slots[0].proc.exit_status;
			break;
		}
		k_msleep(1);
	}
	g_lnx_active = 0;
	return rc;
}
