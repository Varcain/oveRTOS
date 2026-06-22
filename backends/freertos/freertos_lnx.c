/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * FreeRTOS engine seam for the Linux personality (see include/ove/linux/run.h).
 * The FreeRTOS analogue of backends/zephyr/zephyr_lnx.c: it traps the loaded
 * program's `svc #0`, runs the NOMMU process model (sequentialised vfork/exec/
 * wait + signal delivery + the run loop) on FreeRTOS tasks, and dispatches into
 * the engine-agnostic syscall core (ove_lnx_syscall).
 *
 * PHASE 1 (functional parity): the program runs as a normal PRIVILEGED FreeRTOS
 * task on the non-MPU ARM_CM7 port. Its `svc #0` still takes the SVCall
 * exception, which this seam OWNS: the board's FreeRTOSConfig.h does NOT alias
 * vPortSVCHandler->SVC_Handler, so the strong SVC_Handler below is the vector;
 * it dispatches the program's svc (while g_lnx_active) to the personality handler
 * and forwards FreeRTOS's own start-scheduler svc to vPortSVCHandler. (FreeRTOS's
 * non-MPU port only svc's once, at scheduler start, before any ove_lnx_run;
 * afterwards it context-switches via PendSV, so svc is the program's.) Phase 2
 * will switch to the ARM_CM4_MPU port to run the program unprivileged + isolated.
 */

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#include "ove/arena.h"
#include "ove/loader.h"
#include "ove/linux/run.h"
#include "ove/linux/syscall.h"

/* ---- program memory: two regions, so a parent + child image coexist -------- */
#define PROG_REGION_SIZE 0x60000u /* 384K: BusyBox loads ~129K + arena + stack */
#define PROG_ARENA_SIZE 0x18000u  /* 96K heap for the program */
#define NREG 2
#define NSLOT 2
#define TRAMP_STACK_WORDS 256u		  /* the FreeRTOS task stack for the tramp prologue */
#define SLOT_PRIO (tskIDLE_PRIORITY + 1u) /* below the run-loop task (its creator) */

static uint8_t prog_regions[NREG][PROG_REGION_SIZE] __attribute__((aligned(32)));
static ove_arena_t g_arenas[NREG];

/* ---- per-process slots (FreeRTOS tasks) ------------------------------------ */
struct slot {
	ove_lnx_proc_t proc;
	TaskHandle_t tid;
	StaticTask_t tcb;
	int used;
};
static struct slot g_slots[NSLOT];
static StackType_t g_tramp_stacks[NSLOT][TRAMP_STACK_WORDS] __attribute__((aligned(8)));

static const ove_lnx_run_config_t *g_cfg;
static volatile int g_lnx_active;

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

/* ---- the SVC trap ---------------------------------------------------------- */
/* The exception frame as the seam sees it: a pointer to the HW-stacked frame
 * (live on the program PSP, so writes ARE the rewrite) + the callee-saved
 * registers captured by the asm shim before they are clobbered. */
struct lnx_frame {
	uint32_t *hw;	   /* hw[0..7] = r0,r1,r2,r3,r12,lr,pc,xpsr */
	uint32_t psp;	   /* the program SP at the HW frame */
	uint32_t r4_11[8]; /* r4..r11 */
};
static struct lnx_frame g_frame __attribute__((used)); /* referenced from SVC_Handler asm */

static void park_loop(void)
{
	for (;;) {
	}
}

static struct slot *current_slot(void)
{
	TaskHandle_t t = xTaskGetCurrentTaskHandle();
	for (int i = 0; i < NSLOT; i++)
		if (g_slots[i].used && g_slots[i].tid == t)
			return &g_slots[i];
	return NULL;
}

/* Deliver signal `sig` to slot `s`; `ret` is the interrupted syscall's result. */
static void deliver_signal(struct lnx_frame *f, struct slot *s, int sig, long ret)
{
	if (sig < 1 || sig >= OVE_LNX_NSIG) {
		f->hw[0] = (uint32_t)-OVE_LNX_EINVAL;
		return;
	}
	uintptr_t h = s->proc.sig_handler[sig];
	if (h == OVE_LNX_SIG_IGN) {
		f->hw[0] = (uint32_t)ret;
		return;
	}
	if (h == OVE_LNX_SIG_DFL) {
		s->proc.exited = 1;
		s->proc.exit_status = 128 + sig;
		f->hw[6] = ((uint32_t)&park_loop) | 1u;
		return;
	}
	g_sig_save.r0 = (uint32_t)ret;
	g_sig_save.r1 = f->hw[1];
	g_sig_save.r2 = f->hw[2];
	g_sig_save.r3 = f->hw[3];
	g_sig_save.r12 = f->hw[4];
	g_sig_save.lr = f->hw[5];
	g_sig_save.pc = f->hw[6];
	g_sig_save.xpsr = f->hw[7];
	g_sig_save.active = 1;
	f->hw[6] = h | 1u;			   /* pc -> handler */
	f->hw[0] = (uint32_t)sig;		   /* r0 = signo */
	f->hw[5] = s->proc.sig_restorer[sig] | 1u; /* lr -> sa_restorer */
}

static void sig_restore(struct lnx_frame *f)
{
	if (!g_sig_save.active)
		return;
	f->hw[0] = g_sig_save.r0;
	f->hw[1] = g_sig_save.r1;
	f->hw[2] = g_sig_save.r2;
	f->hw[3] = g_sig_save.r3;
	f->hw[4] = g_sig_save.r12;
	f->hw[5] = g_sig_save.lr;
	f->hw[6] = g_sig_save.pc;
	f->hw[7] = g_sig_save.xpsr;
	g_sig_save.active = 0;
}

/* The C body of the svc trap (mirrors __wrap_z_do_kernel_oops). */
void freertos_lnx_svc_c(struct lnx_frame *f)
{
	struct slot *s = current_slot();
	if (!s)
		return;
	long nr = (long)f->r4_11[3]; /* r7 = the Linux syscall number */

	/* Track the tty ISIG mode so console ^C knows whether to raise SIGINT. */
	if (nr == OVE_LNX_NR_ioctl) {
		unsigned long cmd = f->hw[1];
		if (cmd == OVE_LNX_TCSETS || cmd == OVE_LNX_TCSETSW || cmd == OVE_LNX_TCSETSF) {
			const ove_lnx_termios *t = (const ove_lnx_termios *)(uintptr_t)f->hw[2];
			if (t)
				g_tty_isig = (t->c_lflag & OVE_LNX_ISIG) ? 1 : 0;
		}
	}
	if (nr == OVE_LNX_NR_kill || nr == OVE_LNX_NR_tkill || nr == OVE_LNX_NR_tgkill) {
		int sig = (nr == OVE_LNX_NR_tgkill) ? (int)f->hw[2] : (int)f->hw[1];
		deliver_signal(f, s, sig, 0);
		return;
	}
	if (nr == OVE_LNX_NR_rt_sigreturn || nr == OVE_LNX_NR_sigreturn) {
		sig_restore(f);
		return;
	}
	if (nr == OVE_LNX_NR_vfork || nr == OVE_LNX_NR_fork) {
		for (int i = 0; i < 8; i++)
			g_vfork_ctx.r4_11[i] = f->r4_11[i];
		g_vfork_ctx.r12 = f->hw[4];
		g_vfork_ctx.lr = f->hw[5];
		/* The HW frame is 32 bytes (8 words); +4 if the stacker aligned. */
		g_vfork_ctx.sp = f->psp + 32u + ((f->hw[7] & (1u << 9)) ? 4u : 0u);
		g_vfork_ctx.pc = f->hw[6] | 1u;
		g_vfork_pending = 1;
		f->hw[6] = ((uint32_t)&park_loop) | 1u;
		return;
	}
	long r = ove_lnx_syscall(&s->proc, nr, (int32_t)f->hw[0], (int32_t)f->hw[1],
				 (int32_t)f->hw[2], (int32_t)f->hw[3], (int32_t)f->r4_11[0],
				 (int32_t)f->r4_11[1]);
	if (r == -OVE_LNX_ENOSYS && g_cfg && g_cfg->on_enosys)
		g_cfg->on_enosys(nr);
	if (s->proc.exited || s->proc.exec_pending) {
		f->hw[6] = ((uint32_t)&park_loop) | 1u;
		return;
	}
	if (g_pending_sig) {
		int sig = g_pending_sig;
		g_pending_sig = 0;
		deliver_signal(f, s, sig, r);
		return;
	}
	f->hw[0] = (uint32_t)r;
}

/* FreeRTOS's own SVC handler (start scheduler); not aliased to SVC_Handler. */
extern void vPortSVCHandler(void);

/* SVC vector: dispatch the program's svc (while active) to the personality, else
 * forward to FreeRTOS. Capture r4-r11 (live at entry) + the HW frame pointer. */
__attribute__((naked)) void SVC_Handler(void)
{
	__asm__ volatile("ldr   r1, =g_lnx_active   \n"
			 "ldr   r1, [r1]            \n"
			 "cmp   r1, #0              \n"
			 "beq   1f                  \n" /* inactive -> FreeRTOS */
			 "mrs   r0, psp             \n" /* r0 = HW exception frame */
			 "ldr   r1, =g_frame        \n"
			 "str   r0, [r1, #0]        \n" /* g_frame.hw  */
			 "str   r0, [r1, #4]        \n" /* g_frame.psp */
			 "add   r2, r1, #8          \n"
			 "stmia r2, {r4-r11}        \n" /* g_frame.r4_11 */
			 "mov   r0, r1              \n"
			 "push  {lr}                \n"
			 "bl    freertos_lnx_svc_c  \n"
			 "pop   {lr}                \n"
			 "bx    lr                  \n" /* exception return: replay frame */
			 "1:                        \n"
			 "b     vPortSVCHandler     \n");
}

/* ---- thread entry trampolines ---------------------------------------------- */
struct launch_args {
	void *sp;
	void *entry;
};
struct resume_args {
	long r0;
};
static struct launch_args g_largs[NSLOT];
static struct resume_args g_rargs[NSLOT];

/* Enter a fresh program: SP -> argc block, r0 = 0 (static fini). Naked: it
 * switches sp and never returns, so no compiler prologue/epilogue. */
__attribute__((naked)) static void arg_tramp(void *sp __attribute__((unused)),
					     void *entry __attribute__((unused)))
{
	/* AAPCS: r0 = sp, r1 = entry. */
	__asm__ volatile("mov sp, r0\n"
			 "mov r0, #0\n"
			 "bx  r1\n");
}
static void arg_tramp_task(void *arg)
{
	struct launch_args *a = (struct launch_args *)arg;
	arg_tramp(a->sp, a->entry);
}

/* Resume a parked program at a captured context with a chosen r0 (vfork return).
 * Naked: it restores callee-saved regs (incl. r7, the frame pointer) + sp and
 * branches to the saved pc, so it cannot be a normal function. */
__attribute__((naked)) static void resume_tramp(void *r0val __attribute__((unused)),
						void *ctx __attribute__((unused)))
{
	/* AAPCS: r0 = r0val (kept as the resumed program's r0), r1 = ctx. */
	__asm__ volatile("ldmia r1!, {r4-r11}\n"
			 "ldr   r12, [r1], #4\n"
			 "ldr   lr, [r1], #4\n"
			 "ldr   sp, [r1], #4\n"
			 "ldr   r1, [r1]\n"
			 "bx    r1\n");
}
static void resume_tramp_task(void *arg)
{
	struct resume_args *a = (struct resume_args *)arg;
	resume_tramp((void *)a->r0, &g_vfork_ctx);
}

/* ---- slot lifecycle -------------------------------------------------------- */
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
	if (!sp)
		return -1;
	g_largs[sidx].sp = sp;
	g_largs[sidx].entry = (void *)prog.entry;
	g_slots[sidx].tid = xTaskCreateStatic(arg_tramp_task, "lnx", TRAMP_STACK_WORDS,
					      &g_largs[sidx], SLOT_PRIO, g_tramp_stacks[sidx],
					      &g_slots[sidx].tcb);
	g_slots[sidx].used = 1;
	return g_slots[sidx].tid ? 0 : -1;
}

static void resume_slot(int sidx, int ridx, long r0val)
{
	(void)ridx;
	g_rargs[sidx].r0 = r0val;
	g_slots[sidx].tid = xTaskCreateStatic(resume_tramp_task, "lnx", TRAMP_STACK_WORDS,
					      &g_rargs[sidx], SLOT_PRIO, g_tramp_stacks[sidx],
					      &g_slots[sidx].tcb);
	g_slots[sidx].used = 1;
}

int ove_lnx_run(const ove_lnx_run_config_t *cfg, const char *path, int argc,
		const char *const argv[])
{
	if (!cfg || !cfg->rootfs || !path || argc < 1 || !argv)
		return OVE_LNX_RUN_ELAUNCH;
	g_cfg = cfg;
	for (int i = 0; i < NSLOT; i++)
		g_slots[i].used = 0;
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

	g_lnx_active = 1;
	if (launch_slot(0, 0, cfg->rootfs[bb].data, cfg->rootfs[bb].size, 1, 0, argc, argv) != 0) {
		g_lnx_active = 0;
		return OVE_LNX_RUN_ELAUNCH;
	}

	int rc = OVE_LNX_RUN_ETIMEOUT;
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
			vTaskDelete(g_slots[0].tid);
			g_slots[0].used = 0;
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
			vTaskDelete(g_slots[1].tid);
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
			vTaskDelete(g_slots[1].tid);
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
		if (g_slots[0].used && g_slots[0].proc.exited) {
			rc = g_slots[0].proc.exit_status;
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	for (int i = 0; i < NSLOT; i++) {
		if (g_slots[i].used) {
			vTaskDelete(g_slots[i].tid);
			g_slots[i].used = 0;
		}
	}
	g_lnx_active = 0;
	return rc;
}
