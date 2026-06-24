/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr seam for the Linux personality. The engine-agnostic run loop, svc
 * dispatch, and signal delivery live in backends/common/ove_lnx_run.c; this file
 * supplies only the Zephyr-specific bits: the svc trap, the program memory + MPU
 * domains, and the task spawn (via the ove_lnx_engine vtable).
 *
 * Each program runs as an UNPRIVILEGED K_USER thread in its own k_mem_domain
 * (program text/data partitions, W^X, + libc/heap), so the privileged run loop
 * (default domain) can always (re)load a region. The program's svc #0 is an
 * unprivileged fault that Zephyr routes to z_do_kernel_oops, which we --wrap.
 */

#include <zephyr/kernel.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/linker/devicetree_regions.h>
#include <string.h>

#include "../common/ove_lnx_run.h"

/* The program-image regions live in a NOLOAD PSRAM linker region (the board's
 * ove-psram.overlay turns the AN521 PSRAM into "OVE_PROG_RAM"): RAM-resident but
 * ZERO flash cost — Zephyr's app_smem is a *loaded* section, so a K_APP_BMEM array
 * this big would store 2 MB of zero-init regions as 2 MB of flash. PSRAM is also a
 * region separate from the kernel SRAM, so the per-program MPU partitions built in
 * setup_domain() don't overlap the kernel's region (the reason app_smem was used). */
static uint8_t prog_regions[OVE_LNX_NREG][OVE_LNX_PROG_REGION_SIZE] Z_GENERIC_SECTION(
	LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(psram))) __aligned(32);

/* A user-readable partition (in every program domain) for the vfork resume
 * context the unprivileged resume thread replays. The common capture buffer is
 * privileged kernel .bss, so spawn_resume copies it here first. */
K_APPMEM_PARTITION_DEFINE(ove_lnx_shared_partition);
K_APP_BMEM(ove_lnx_shared_partition) static struct ove_lnx_resume_ctx g_vfork_user;

/* Per-region MPU domain: program text/data + the libc/heap partitions. */
extern struct k_mem_partition z_libc_partition;
extern struct k_mem_partition z_malloc_partition;
static struct k_mem_domain g_domains[OVE_LNX_NREG];
static struct k_mem_partition g_text[OVE_LNX_NREG], g_data[OVE_LNX_NREG];
static int g_dom_inited[OVE_LNX_NREG];

static struct k_thread g_thread[OVE_LNX_NSLOT];
static k_tid_t g_tid[OVE_LNX_NSLOT];
K_THREAD_STACK_ARRAY_DEFINE(g_tramp_stacks, OVE_LNX_NSLOT, 1024);

static int current_slot(void)
{
	k_tid_t t = k_current_get();
	for (int i = 0; i < OVE_LNX_NSLOT; i++)
		if (g_ove_lnx_used[i] && g_tid[i] == t)
			return i;
	return -1;
}

/* ---- the Linux SVC seam ---------------------------------------------------- */
extern void __real_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
				    uint32_t exc_return);

void __wrap_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
			     uint32_t exc_return)
{
	if (g_ove_lnx_active) {
		const uint16_t *svc = (const uint16_t *)(esf->basic.pc - 2);
		if ((*svc & 0xff00u) == 0xdf00u && (*svc & 0x00ffu) == 0x00u) {
			int sidx = current_slot();
			if (sidx >= 0) {
				struct arch_esf *e = (struct arch_esf *)esf;
				struct ove_lnx_frame f;
				f.r[0] = esf->basic.r0;
				f.r[1] = esf->basic.r1;
				f.r[2] = esf->basic.r2;
				f.r[3] = esf->basic.r3;
				f.r[4] = callee->v1;
				f.r[5] = callee->v2;
				f.r[6] = callee->v3;
				f.r[7] = callee->v4;
				f.r[8] = callee->v5;
				f.r[9] = callee->v6;
				f.r[10] = callee->v7;
				f.r[11] = callee->v8;
				f.r[12] = esf->basic.ip;
				/* callee->psp points at the HW-stacked frame (8 words); the
				 * pre-svc SP is +32 (+4 if the stacker 8-byte-aligned). */
				f.r[13] = callee->psp + 32u +
					  ((esf->basic.xpsr & (1u << 9)) ? 4u : 0u);
				f.r[14] = esf->basic.lr;
				f.r[15] = esf->basic.pc;
				f.xpsr = esf->basic.xpsr;

				ove_lnx_dispatch(&f, &g_ove_lnx_proc[sidx]);

				e->basic.r0 = f.r[0];
				e->basic.r1 = f.r[1];
				e->basic.r2 = f.r[2];
				e->basic.r3 = f.r[3];
				e->basic.ip = f.r[12];
				e->basic.lr = f.r[14];
				e->basic.pc = f.r[15];
				e->basic.xpsr = f.xpsr;
				return;
			}
		}
	}
	__real_z_do_kernel_oops(esf, callee, exc_return);
}

/* ---- thread entry trampolines ---------------------------------------------- */
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
	__asm__ volatile("ldmia r1!, {r4-r11}\n"
			 "ldr r12, [r1], #4\n"
			 "ldr lr, [r1], #4\n"
			 "ldr sp, [r1], #4\n"
			 "ldr r1, [r1]\n"
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
	g_data[ridx].size = OVE_LNX_PROG_REGION_SIZE - prog->text_size;
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

/* ---- the vtable: Zephyr task spawn ----------------------------------------- */
static uint8_t *zephyr_region(int ridx)
{
	return prog_regions[ridx];
}

static int zephyr_spawn_launch(int sidx, int ridx, const ove_flat_t *prog, void *entry, void *sp,
			       void *stack_lo)
{
	ARG_UNUSED(stack_lo);
	if (setup_domain(ridx, prog) != 0)
		return -1;
	g_tid[sidx] = k_thread_create(&g_thread[sidx], g_tramp_stacks[sidx],
				      K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]), arg_tramp, sp,
				      entry, NULL, 5, K_USER, K_FOREVER);
	k_thread_name_set(g_tid[sidx], "lnx"); /* ps/top: classify as a Linux program */
	g_ove_lnx_used[sidx] = 1;
	k_mem_domain_add_thread(&g_domains[ridx], g_tid[sidx]);
	k_thread_start(g_tid[sidx]);
	return 0;
}

static void zephyr_spawn_resume(int sidx, int ridx, long r0val)
{
	g_vfork_user = g_ove_lnx_vfork; /* copy to the user-readable partition */
	g_tid[sidx] = k_thread_create(&g_thread[sidx], g_tramp_stacks[sidx],
				      K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]), resume_tramp,
				      (void *)r0val, &g_vfork_user, NULL, 5, K_USER, K_FOREVER);
	k_thread_name_set(g_tid[sidx], "lnx"); /* ps/top: classify as a Linux program */
	g_ove_lnx_used[sidx] = 1;
	k_mem_domain_add_thread(&g_domains[ridx], g_tid[sidx]);
	k_thread_start(g_tid[sidx]);
}

static void zephyr_abort_slot(int sidx)
{
	if (g_ove_lnx_used[sidx] && g_tid[sidx])
		k_thread_abort(g_tid[sidx]);
	g_ove_lnx_used[sidx] = 0;
}

static void zephyr_sleep_ms(unsigned ms)
{
	k_msleep((int32_t)ms);
}

static const struct ove_lnx_engine g_zephyr_engine = {
	.region = zephyr_region,
	.spawn_launch = zephyr_spawn_launch,
	.spawn_resume = zephyr_spawn_resume,
	.abort_slot = zephyr_abort_slot,
	.sleep_ms = zephyr_sleep_ms,
};

int ove_lnx_run(const ove_lnx_run_config_t *cfg, const char *path, int argc,
		const char *const argv[])
{
	return ove_lnx_run_common(&g_zephyr_engine, cfg, path, argc, argv);
}
