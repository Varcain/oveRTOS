/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr Linux-personality on-target test (mps2/an521/cpu0, Cortex-M33): the
 * process model — vfork + execve + wait4 — exercised by a real uClibc-ng shell
 * bFLT. The shell vfork()s, the child execve()s /bin/hello2, the parent
 * waitpid()s and prints, all unprivileged.
 *
 * NOMMU process model on Zephyr (sequentialised, which is observationally
 * identical to vfork for the shell pattern since the parent waitpid()s anyway):
 *  - vfork: the seam captures the parent's full resume context (r4-r11, r12, lr,
 *    sp, pc at the svc return) and parks the parent. main spawns a CHILD thread
 *    that resumes at that context with r0=0 (the child's vfork return), sharing
 *    the parent's region/domain until it execs.
 *  - the child execve()s -> main loads the new image into a SECOND region/domain
 *    and runs it (image replacement, as in the execve test).
 *  - the child exits -> main records its status into the parent, then resumes
 *    the parent thread at the same captured context with r0=child_pid.
 *  - the parent waitpid()s -> the child is already reaped, so wait4 returns the
 *    pid + status; the parent prints and exits.
 *
 * Each program runs in its OWN k_mem_domain (program partitions + z_libc/z_malloc)
 * so privileged main (default domain) can always (re)load a region. I/O + exit
 * go through ARM semihosting.
 */

#include <zephyr/kernel.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <string.h>

#include "ove/arena.h"
#include "ove/linux/syscall.h"
#include "ove/loader.h"

#include "loader_hello_image.h"	 /* ove_test_hello_bflt[], _len  (the vfork shell) */
#include "loader_hello2_image.h" /* ove_test_hello2_bflt[], _len (the exec'd child) */

/* ARM semihosting (host console + clean QEMU exit). */
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

/* ---- program memory: two regions, so a parent + child image coexist -------- */
#define PROG_REGION_SIZE 0x30000u
#define PROG_ARENA_SIZE 0x10000u
#define NREG 2
K_APPMEM_PARTITION_DEFINE(prog_partition);
K_APP_BMEM(prog_partition) static uint8_t prog_regions[NREG][PROG_REGION_SIZE] __aligned(32);
static ove_arena_t g_arenas[NREG];

/* A user-readable partition (in every program domain) for the vfork resume
 * context, which the unprivileged resume thread reads to replay its registers.
 * Kernel .bss would be privileged-only and fault under the K_USER thread. */
K_APPMEM_PARTITION_DEFINE(shared_partition);

/* Per-region MPU domain: program text/data + the libc/heap partitions. */
extern struct k_mem_partition z_libc_partition;
extern struct k_mem_partition z_malloc_partition;
static struct k_mem_domain g_domains[NREG];
static struct k_mem_partition g_text[NREG], g_data[NREG];
static int g_dom_inited[NREG];

/* ---- output sink ----------------------------------------------------------- */
static char g_cap[128];
static volatile size_t g_cap_len;

static long capture_write(void *ctx, int fd, const void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	if (g_cap_len + len > sizeof(g_cap))
		len = sizeof(g_cap) - g_cap_len;
	memcpy(g_cap + g_cap_len, buf, len);
	g_cap_len += len;
	return (long)len;
}

/* Scripted stdin: the shell read(0)s these command lines as if typed at a
 * console (the engine is the terminal). Deterministic, so no live-TTY flake. */
static const char g_input[] = "hello2\nhello2\nexit\n";
static volatile size_t g_input_pos;

static long console_read(void *ctx, int fd, void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	size_t avail = sizeof(g_input) - 1 - g_input_pos;
	if (avail == 0)
		return 0; /* EOF */
	if (len > avail)
		len = avail;
	memcpy(buf, g_input + g_input_pos, len);
	g_input_pos += len;
	return (long)len;
}

/* ---- per-process slots ----------------------------------------------------- */
#define NSLOT 2
struct slot {
	ove_lnx_proc_t proc;
	struct k_thread thread;
	k_tid_t tid;
	int used;
};
static struct slot g_slots[NSLOT];
K_THREAD_STACK_ARRAY_DEFINE(g_tramp_stacks, NSLOT, 1024);

static volatile int g_lnx_active;

/* Captured parent context at a vfork svc, replayed to resume parent + child. */
struct resume_ctx {
	uint32_t r4_11[8];
	uint32_t r12;
	uint32_t lr;
	uint32_t sp;
	uint32_t pc;
};
K_APP_BMEM(shared_partition) static struct resume_ctx g_vfork_ctx;
static volatile int g_vfork_pending;

/* Where a program parks (in its own text) after a blocking syscall, until main
 * reaps/relaunches/resumes it. */
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
				if (s->proc.exited || s->proc.exec_pending) {
					((struct arch_esf *)esf)->basic.pc =
						((uint32_t)&park_loop) | 1u;
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

/* ---- rootfs ---------------------------------------------------------------- */
static const ove_lnx_file_t g_rootfs[] = {
	{"/bin", NULL, 0, OVE_LNX_S_IFDIR},
	{"/bin/hello2", ove_test_hello2_bflt, sizeof(ove_test_hello2_bflt), 0},
};
#define G_ROOTFS_N ((int)(sizeof(g_rootfs) / sizeof(g_rootfs[0])))

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
						  &shared_partition};
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
	g_slots[sidx].proc.write_fn = capture_write;
	g_slots[sidx].proc.read_fn = console_read;
	g_slots[sidx].proc.pid = pid;
	g_slots[sidx].proc.ppid = ppid;
	ove_lnx_proc_set_rootfs(&g_slots[sidx].proc, g_rootfs, G_ROOTFS_N);
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

/* The shell reads "hello2\nhello2\nexit\n" from stdin, spawning /bin/hello2 for
 * each line (prompt "$ " before each read), then "bye" on exit. */
#define EXPECT_MSG "$ execed: hello2\n$ execed: hello2\n$ bye\n"

int main(void)
{
	sh_write0("=== Zephyr uClibc vfork/execve/wait personality test (an521) ===\n");

	g_lnx_active = 1;
	const char *const sh_argv[] = {"sh", NULL};
	if (launch_slot(0, 0, ove_test_hello_bflt, ove_test_hello_bflt_len, 1, 0, 1, sh_argv) !=
	    0) {
		sh_write0("[zephyr-linux] FAIL: shell launch failed\n");
		sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
		sh_exit(1);
	}

	int ok = 0;
	int next_pid = 2;  /* assigned to each new child */
	int cur_child = 2; /* the child currently being spawned/reaped */
	for (int i = 0; i < 8000; i++) {
		/* The shell vfork()ed: spawn the child resuming at vfork (r0=0),
		 * sharing the parent's region/domain until it execs; park the
		 * parent (abort its thread — it is replayed after the child exits). */
		if (g_vfork_pending) {
			g_vfork_pending = 0;
			/* vfork shares the parent's address space: the child's proc is
			 * a copy of the parent's (same arena/fds/rootfs) with its own
			 * process identity and cleared exec/exit/reap latches. */
			cur_child = next_pid++;
			g_slots[1].proc = g_slots[0].proc;
			g_slots[1].proc.pid = cur_child;
			g_slots[1].proc.ppid = 1;
			g_slots[1].proc.exited = 0;
			g_slots[1].proc.exec_pending = 0;
			g_slots[1].proc.child_exited = 0;
			k_thread_abort(g_slots[0].tid);
			g_slots[0].used = 0; /* parent parked; resumed after child exit */
			resume_slot(1, 0, 0);
			continue;
		}
		/* The child execve()d: load /bin/hello2 into region 1 + run it. */
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
			k_thread_abort(g_slots[1].tid);
			if (launch_slot(1, 1, g_rootfs[idx].data, g_rootfs[idx].size, cur_child, 1,
					eargc, ptrs) != 0) {
				sh_write0("[zephyr-linux] FAIL: child execve relaunch failed\n");
				sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
				sh_exit(1);
			}
			continue;
		}
		/* The child exited: record its status for wait4, then resume the
		 * parent at the vfork context with r0 = child_pid. */
		if (g_slots[1].used && g_slots[1].proc.exited) {
			int status = g_slots[1].proc.exit_status;
			k_thread_abort(g_slots[1].tid);
			g_slots[1].used = 0;
			/* Restore the parent into slot 0, region 0 (intact). */
			g_slots[0].proc.child_pid = cur_child;
			g_slots[0].proc.child_status = status;
			g_slots[0].proc.child_exited = 1;
			resume_slot(0, 0, cur_child);
			continue;
		}
		/* The parent (shell) exited: done. */
		if (g_slots[0].used && g_slots[0].proc.exited) {
			ok = 1;
			break;
		}
		k_msleep(1);
	}
	g_lnx_active = 0;

	if (ok && g_slots[0].proc.exit_status == 0 && g_cap_len == sizeof(EXPECT_MSG) - 1 &&
	    memcmp(g_cap, EXPECT_MSG, g_cap_len) == 0) {
		sh_write0("[zephyr-linux] interactive shell read commands from stdin + spawned "
			  "each (vfork/execve/wait), parent survived OK\n");
		sh_write0("\n=== Summary: 0 test group(s) had failures ===\n");
		sh_exit(0);
	}

	sh_write0("[zephyr-linux] FAIL: ok=");
	{
		char b[2] = {(char)('0' + (ok & 1)), 0};
		sh_write0(b);
	}
	sh_write0(" status/out follow\n  out=");
	if (g_cap_len) {
		g_cap[g_cap_len < sizeof(g_cap) ? g_cap_len : sizeof(g_cap) - 1] = 0;
		sh_write0(g_cap);
	}
	sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
	sh_exit(1);
	return 0;
}
