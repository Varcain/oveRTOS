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
 *
 * MPU REGION BUDGET (the source of a since-fixed hang): the AN521 PMSAv8 MPU has
 * only 8 regions; 2 are static (kernel) leaving 6 dynamic, and a K_USER thread also
 * needs a stack region. So each program domain may use at most FOUR partitions
 * (libc + malloc + text + data) — a fifth (a dedicated shared partition for the
 * vfork resume ctx), once a NON_OVERLAPPING split bumped the live count past 6,
 * silently overflowed (CONFIG_ASSERT is off in this build) and dropped the
 * kernel-text region, so a parked program took an instruction-access MemManage
 * fault in park_loop after a handful of fork+exec cycles → kernel panic → halt-loop
 * (looked like a hang). The resume ctx now rides in the program's own region just
 * below its resume SP (see zephyr_spawn_resume), keeping the domain at 4 partitions.
 * Found via on-target GDB (reason=K_ERR_ARM_MEM_INSTRUCTION_ACCESS at &park_loop).
 */

#include <zephyr/kernel.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/linker/devicetree_regions.h>
#include <string.h>

#include "../common/ove_lnx_run.h"
#include "ove/time.h"	/* ove_time_get_us/ns -> engine time_us/time_ns ops */
#include "ove/thread.h" /* ove_thread_list -> engine thread_list op */

/* The program-image regions live in a NOLOAD external-RAM linker region: RAM-resident but ZERO
 * flash cost — Zephyr's app_smem is a *loaded* section, so a K_APP_BMEM array this big would store
 * the zero-init regions as that many MB of flash. External RAM is also a region separate from the
 * kernel SRAM, so the per-program MPU partitions built in setup_domain() don't overlap the kernel's
 * region (the reason app_smem was used). The node differs per board: an521 uses the 16 MB PSRAM
 * (ove-psram.overlay → "OVE_PROG_RAM"); the real STM32F746 uses the 8 MB FMC SDRAM @0xC0000000
 * (upstream `sdram1` node → "SDRAM1"; the LTDC display that would share it is disabled in the
 * linux_interop overlay). */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#define OVE_PROG_RAM_NODE DT_NODELABEL(sdram1)
#else
#define OVE_PROG_RAM_NODE DT_NODELABEL(psram)
#endif
/* Per-program partition base alignment. On a power-of-2 MPU (PMSAv7, e.g. the STM32F746, where
 * CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT=y) an MPU region's base must be aligned to its SIZE, so
 * each per-program partition (one row of these arrays) must start on a PROG_REGION/DYN_POOL boundary
 * — otherwise the hardware aligns the base DOWN at context switch and the region spans the wrong
 * range, so the unprivileged program's data/IO lands in unmapped memory (it launches but silently
 * relays nothing). PMSAv8 (the an521) allows a 32-byte-aligned base, so it needs no size alignment
 * (and avoids the padding). */
#if defined(CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT)
#define OVE_LNX_PROG_REGION_ALIGN OVE_LNX_PROG_REGION_SIZE
#define OVE_LNX_DYN_POOL_ALIGN OVE_LNX_DYN_POOL_SIZE
#else
#define OVE_LNX_PROG_REGION_ALIGN 32
#define OVE_LNX_DYN_POOL_ALIGN 32
#endif
static uint8_t prog_regions[OVE_LNX_NREG][OVE_LNX_PROG_REGION_SIZE] Z_GENERIC_SECTION(
	LINKER_DT_NODE_REGION_NAME(OVE_PROG_RAM_NODE)) __aligned(OVE_LNX_PROG_REGION_ALIGN);
/* Per-region dynamic-link scratch pool (also external-RAM/NOLOAD): a dynamic FDPIC proc's arena
 * lives here so ld.so can mmap libc.so (~500K), far past the in-region arena. */
static uint8_t dyn_pools[OVE_LNX_NREG][OVE_LNX_DYN_POOL_SIZE] Z_GENERIC_SECTION(
	LINKER_DT_NODE_REGION_NAME(OVE_PROG_RAM_NODE)) __aligned(OVE_LNX_DYN_POOL_ALIGN);

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* The Linux personality console (apps/.../app.c, the CONFIG_OVE_BOARD_STM32F746G_DISCO branch)
 * polls USART1 directly from the PRIVILEGED run loop. Zephyr already brought USART1 up as its own
 * console; we only steal RX from its IRQ path: serial_poll_begin() clears RXNEIE so the polled
 * reads own the receiver. STM32F7 USART1 @ 0x40011000: CR1(0x00) RXNEIE=b5, ISR(0x1C) RXNE=b5
 * TXE=b7, RDR(0x24), TDR(0x28). */
#define OVE_Z_USART1 0x40011000u
#define OVE_Z_U1_CR1 (*(volatile uint32_t *)(OVE_Z_USART1 + 0x00u))
#define OVE_Z_U1_ISR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x1Cu))
#define OVE_Z_U1_RDR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x24u))
#define OVE_Z_U1_TDR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x28u))
void serial_poll_begin(void)
{
	OVE_Z_U1_CR1 &= ~(1u << 5); /* clear RXNEIE → polled access owns RX */
}
int serial_poll_rx_ready(void)
{
	return (OVE_Z_U1_ISR & (1u << 5)) ? 1 : 0; /* RXNE */
}
int serial_poll_getc(void)
{
	return (int)(OVE_Z_U1_RDR & 0xFFu);
}
void serial_poll_putc(char c)
{
	while (!(OVE_Z_U1_ISR & (1u << 7))) { /* wait for TXE */
	}
	OVE_Z_U1_TDR = (unsigned char)c;
}
#endif

/* Per-region MPU domain: program text/data + the libc/heap partitions. */
extern struct k_mem_partition z_libc_partition;
extern struct k_mem_partition z_malloc_partition;
static struct k_mem_domain g_domains[OVE_LNX_NREG];
static struct k_mem_partition g_text[OVE_LNX_NREG], g_data[OVE_LNX_NREG];
static int g_dom_inited[OVE_LNX_NREG];

/* Guest program pool: Normal write-back write-allocate, NON-shareable, CACHEABLE — deliberately the
 * SAME memory attribute the privileged run loop sees the FMC SDRAM through (Zephyr's static SDRAM1
 * MPU region, DT ATTR_MPU_RAM = REGION_RAM_ATTR = WBWA non-shareable). Coordinator and guest MUST
 * agree on cacheability: the run loop writes into this pool PRIVILEGED (the loader's ELF/RW load,
 * syscall result buffers, argv/auxv setup, and the resume ctx that zephyr_spawn_resume stashes just
 * below the guest SP), then the UNPRIVILEGED guest reads it back. This region previously mapped the
 * guest NON-cacheable while the coordinator's view stayed cacheable — a mismatched-attribute alias:
 * with the D-cache on, the coordinator's writes sit in the D-cache and the guest reads stale SDRAM
 * around them → a garbage resume ctx → wild jump → "hang". (D-cache OFF hid it: every write reached
 * SDRAM.) With BOTH sides cacheable on this single M7 core there is ONE coherent cache, so no
 * per-handoff maintenance is needed — unlike FreeRTOS, whose coordinator writes through the uncached
 * background map and therefore must SCB_InvalidateDCache_by_Addr the region before each resume.
 * NON-shareable is essential: the single-core M7 has no snoop unit and precise-BusFaults on
 * shareable Normal FMC accesses. XN — the guest's text XIPs from the RO cpio, never this data pool.
 * Cacheable also speeds the render (the LVGL draw buffer lives here). an521/QEMU has no cache model,
 * so this is a functional no-op there. */
#define OVE_MEM_PART_RW_CACHE K_MEM_PARTITION_P_RW_U_RW

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
	if (prog->is_dynamic) {
		/* Dynamic FDPIC: ALL code (exec + ld.so + libc.so text) is shared IN-PLACE from the
		 * cpio's executable .text subsection (.text.ove_rootfs — user-RX, already covered by the
		 * kernel's static .text MPU region the K_USER thread runs trampolines from), so the
		 * per-process region + arena hold ONLY RW data now. Map them RW — W^X is RESTORED (the
		 * old RWX relaxation, and CONFIG_EXECUTE_XOR_WRITE=n, are gone). region(RW) + arena(RW) +
		 * libc/malloc + the K_USER stack = 5 dynamic MPU regions, the same budget as static. */
		g_text[ridx].start = (uintptr_t)region;
		g_text[ridx].size = OVE_LNX_PROG_REGION_SIZE;
		g_text[ridx].attr = OVE_MEM_PART_RW_CACHE;
		g_data[ridx].start = (uintptr_t)dyn_pools[ridx];
		g_data[ridx].size = OVE_LNX_DYN_POOL_SIZE;
		g_data[ridx].attr = OVE_MEM_PART_RW_CACHE;
	} else {
		g_text[ridx].start = (uintptr_t)region;
		g_text[ridx].size = prog->text_size;
		g_text[ridx].attr = K_MEM_PARTITION_P_RX_U_RX;
		g_data[ridx].start = (uintptr_t)region + prog->text_size;
		g_data[ridx].size = OVE_LNX_PROG_REGION_SIZE - prog->text_size;
		g_data[ridx].attr = OVE_MEM_PART_RW_CACHE;
	}
	if (!g_dom_inited[ridx]) {
#if defined(CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT)
		/* Real STM32F746 (M7, PMSAv7): only 8 MPU regions. After the static set (the board frees
		 * the guard + the QSPI placeholder — see the board prj.conf/overlay) and the K_USER thread's
		 * own stack region, exactly 3 domain partitions fit. z_libc_partition is REQUIRED (Zephyr's
		 * z_thread_entry reads the per-thread TLS pointer z_arm_tls_ptr there via __aeabi_read_tp,
		 * for EVERY thread incl. the program — dropping it faults at entry), but z_malloc_partition
		 * is NOT: the FDPIC program uses its OWN uClibc malloc inside its region and never touches
		 * Zephyr's picolibc heap (the kernel reaches it privileged via PRIVDEFENA). Keep libc, drop
		 * malloc → libc + text + data = 3 partitions. */
		struct k_mem_partition *base[] = {&z_libc_partition};
		if (k_mem_domain_init(&g_domains[ridx], 1, base) != 0)
			return -1;
#else
		/* libc + malloc only (the resume ctx now rides in the program's own region —
		 * see zephyr_spawn_resume — so there is no separate shared partition); text +
		 * data are added below → 4 partitions. The AN521 MPU has 8 regions (2 static →
		 * 6 dynamic) and a K_USER thread also needs a stack region, so staying at 4
		 * partitions (= 5 dynamic) leaves the headroom that a 5th + a NON_OVERLAPPING
		 * split previously overran. */
		struct k_mem_partition *base[] = {&z_libc_partition, &z_malloc_partition};
		if (k_mem_domain_init(&g_domains[ridx], 2, base) != 0)
			return -1;
#endif
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

static uint8_t *zephyr_dyn_pool(int ridx, size_t *size)
{
	if (size)
		*size = OVE_LNX_DYN_POOL_SIZE;
	return dyn_pools[ridx];
}

static int zephyr_spawn_launch(int sidx, int ridx, const ove_flat_t *prog, void *entry, void *sp,
			       void *stack_lo)
{
	ARG_UNUSED(stack_lo);
	if (setup_domain(ridx, prog) != 0)
		return -1;
	if (prog->is_fdpic) {
		/* FDPIC needs r7=exec loadmap, r8=ld.so loadmap, r9=GOT at entry. Rather than a
		 * bespoke unprivileged trampoline, REUSE the proven resume_tramp: synthesize a resume
		 * ctx with the FDPIC registers + the entry as the resume PC, stashed below SP in the
		 * program's own region (where resume_tramp already reads it). r4_11[3..5] = r7/r8/r9
		 * (all 0 for static FDPIC's r8/r9, which the crt overwrites). r0 = 0 (static fini). */
		struct ove_lnx_resume_ctx *slot = (struct ove_lnx_resume_ctx *)((uintptr_t)sp - 64u);
		memset(slot, 0, sizeof(*slot));
		slot->r4_11[3] = (uint32_t)prog->loadmap;	 /* r7 */
		slot->r4_11[4] = (uint32_t)prog->interp_loadmap; /* r8 */
		slot->r4_11[5] = (uint32_t)prog->got;		 /* r9 */
		slot->sp = (uint32_t)(uintptr_t)sp;
		slot->pc = (uint32_t)(uintptr_t)entry;
		g_tid[sidx] = k_thread_create(&g_thread[sidx], g_tramp_stacks[sidx],
					      K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]), resume_tramp,
					      (void *)0, slot, NULL, 5, K_USER, K_FOREVER);
	} else {
		g_tid[sidx] = k_thread_create(&g_thread[sidx], g_tramp_stacks[sidx],
					      K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]), arg_tramp, sp,
					      entry, NULL, 5, K_USER, K_FOREVER);
	}
	{ /* ps/top: "lnx<slot>" classifies as a Linux program + attributes per-process CPU */
		char nm[5] = {'l', 'n', 'x', (char)('0' + sidx), 0};
		k_thread_name_set(g_tid[sidx], nm);
	}
	g_ove_lnx_used[sidx] = 1;
	k_mem_domain_add_thread(&g_domains[ridx], g_tid[sidx]);
	k_thread_start(g_tid[sidx]);
	return 0;
}

static void zephyr_spawn_resume(int sidx, int ridx, const struct ove_lnx_resume_ctx *ctx,
				long r0val)
{
	/* Stash the resume ctx in the program's OWN region (user-RW data), just below the
	 * resume SP, so the unprivileged resume_tramp can read it WITHOUT a dedicated
	 * shared MPU partition. The AN521 MPU has only 6 dynamic region slots and a K_USER
	 * program already needs 5 (libc+malloc+text+data+stack); a 6th shared partition,
	 * once a NON_OVERLAPPING split pushed the count to 7, silently overflowed (CONFIG_
	 * ASSERT off) and dropped the kernel-text region → IACCVIOL in park_loop after a
	 * few fork+exec cycles. The slot is dead stack space (below SP) the program reuses. */
	struct ove_lnx_resume_ctx *slot = (struct ove_lnx_resume_ctx *)((uintptr_t)ctx->sp - 64u);
	*slot = *ctx;
	g_tid[sidx] = k_thread_create(&g_thread[sidx], g_tramp_stacks[sidx],
				      K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]), resume_tramp,
				      (void *)r0val, slot, NULL, 5, K_USER, K_FOREVER);
	{ /* ps/top: "lnx<slot>" classifies as a Linux program + attributes per-process CPU */
		char nm[5] = {'l', 'n', 'x', (char)('0' + sidx), 0};
		k_thread_name_set(g_tid[sidx], nm);
	}
	g_ove_lnx_used[sidx] = 1;
	k_mem_domain_add_thread(&g_domains[ridx], g_tid[sidx]);
	k_thread_start(g_tid[sidx]);
}

/* Coordinator critical section: irq_lock masks SVCall (the program svc is an
 * exception, so k_sched_lock would NOT exclude it). Held only for the brief
 * proc-table flag snapshot — never across abort/spawn (which may yield). */
static unsigned int g_crit_key;
static void zephyr_crit_enter(void)
{
	g_crit_key = irq_lock();
}
static void zephyr_crit_exit(void)
{
	irq_unlock(g_crit_key);
}

/* SCB->ICSR PENDSVSET — raw (0xE000ED04, bit 28), matching the raw-SCS style used elsewhere
 * in the personality seams; avoids a cmsis_core.h include dependency. Writing the whole word
 * is the documented idiom (the other writable ICSR bits are write-1-to-act, so writing 0 to
 * them is a no-op) — Zephyr's own z_arm_exc_exit does `SCB->ICSR = SCB_ICSR_PENDSVSET_Msk`. */
#define OVE_LNX_ICSR (*(volatile uint32_t *)0xE000ED04u)
#define OVE_LNX_PENDSVSET (1u << 28)

/* Event wakeup: the dispatch (fault/exception context) gives this when a program parks; the
 * coordinator takes it instead of busy-polling. ISR-safe k_sem_give. */
K_SEM_DEFINE(g_ove_lnx_ev, 0, 1);
static void zephyr_event_post(void)
{
	k_sem_give(&g_ove_lnx_ev);
	/* The give readies the higher-priority coordinator, but a program svc reaches us via the
	 * kernel-oops path (svc.S .L_oops returns with `pop {r0,pc}`, bypassing z_arm_int_exit),
	 * so nothing pends PendSV — the just-parked K_USER program keeps busy-spinning in
	 * ove_lnx_park_loop until its timeslice expires (~tens of ms), which is the entire cause
	 * of the multi-ms pipe/spawn latency. Pend PendSV ourselves so the coordinator is switched
	 * in on exception return, exactly as z_arm_exc_exit would for a real ISR. A rare no-op
	 * self-switch (nothing higher became ready) is harmless. In thread context (the
	 * coordinator's own cross-kill post) k_sem_give already reschedules, so skip. */
	if (k_is_in_isr()) {
		OVE_LNX_ICSR = OVE_LNX_PENDSVSET;
	}
}
static void zephyr_event_wait(unsigned ms)
{
	k_sem_take(&g_ove_lnx_ev, K_MSEC(ms));
}

/* Contain a program fault — the piece Zephyr lacked vs FreeRTOS/NuttX. A K_USER program that
 * touches memory outside its MPU domain (kernel SRAM, a sibling's region, a wild pointer) raises a
 * fatal MPU fault; Zephyr's default k_sys_fatal_error_handler HALTS the whole system, taking the
 * shell down with it. Override it: when the faulting thread is a Linux program, mark it killed
 * (128 + SIGSEGV = 139) and wake the coordinator, then RETURN — z_fatal_error then aborts only that
 * thread, and the coordinator's EV_EXIT pass reaps it to its parent, so the shell survives (exactly
 * like the FreeRTOS/NuttX MemManage containment handlers). current_slot() reads _current, which the
 * fault has not switched away from, so it still names the faulting program. A fault in privileged
 * runtime code (current_slot() < 0) is a genuine bug → fall through to the halt. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	if (g_ove_lnx_active) {
		int sidx = current_slot();
		if (sidx >= 0) {
			g_ove_lnx_proc[sidx].exited = 1;
			g_ove_lnx_proc[sidx].exit_status = 139; /* 128 + SIGSEGV */
			zephyr_event_post();
			return;
		}
	}
	k_fatal_halt(reason);
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
	.dyn_pool = zephyr_dyn_pool,
	.spawn_launch = zephyr_spawn_launch,
	.spawn_resume = zephyr_spawn_resume,
	.abort_slot = zephyr_abort_slot,
	.sleep_ms = zephyr_sleep_ms,
	.crit_enter = zephyr_crit_enter,
	.crit_exit = zephyr_crit_exit,
	.event_post = zephyr_event_post,
	.event_wait = zephyr_event_wait,
	/* OS-service ops (host adapter). cache_* left NULL: Zephyr's guest memory is
	 * coherent here, matching the former weak no-op ove_lnx_guest_flush. */
	.time_us = ove_time_get_us,
	.time_ns = ove_time_get_ns,
	.thread_list = ove_thread_list,
};

int ove_lnx_run(const ove_lnx_run_config_t *cfg, const char *path, int argc,
		const char *const argv[])
{
	return ove_lnx_run_common(&g_zephyr_engine, cfg, path, argc, argv);
}
