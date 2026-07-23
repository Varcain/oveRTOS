/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * NuttX seam for the Linux personality. The engine-agnostic run loop, svc
 * dispatch, and signal delivery live in modules/lxp/src/lxp_run.c; this file
 * supplies only the NuttX-specific bits: the svc trap, the program memory, and
 * the task spawn (via the lxp_engine vtable).
 *
 * A program's `svc #0` shares the SVCall exception with NuttX's own
 * syscall/context-switch ABI. irq_attach() routes SVCall to our handler, which
 * accepts an SVC as Linux only when the saved context is unprivileged and its
 * running task owns a live personality slot. Privileged NuttX SVCs chain to
 * arm_svcall(); an unprivileged program can never use that chain as an escalation
 * path.
 *
 * Each program runs as a real NuttX task created with nxtask_init() given its own
 * region as the task stack, with the initial register context set to the uClinux
 * entry state (resume replays the captured vfork context).
 *
 * Programs run UNPRIVILEGED behind an MPU view this seam programs itself, in plain
 * CONFIG_BUILD_FLAT: CONTROL.nPRIV is OR'd into each program task's saved CONTROL,
 * lxp_mpu_init() sets the static regions with PRIVDEFENA, a note driver reprograms
 * the per-program regions on every context switch, and a MemManage handler contains
 * the faults. CONFIG_BUILD_PROTECTED is not needed and not used. Verified by
 * qemu-nuttx-linux-segv (kernel RAM) and -xregion (a sibling's pool).
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX)

#include <nuttx/cache.h>     /* up_invalidate_dcache — reused-region coherency (cacheable prog pool) */
#include <nuttx/clock.h>     /* MSEC2TICK */
#include <nuttx/irq.h>	     /* irq_attach, enter/leave_critical_section; arch/irq.h REG_* */
#include <nuttx/sched.h>     /* nxtask_init, nxtask_activate, struct task_tcb_s */
#include <nuttx/semaphore.h> /* nxsem_init/post/tickwait — coordinator wakeup */
#include <nuttx/version.h>
#if defined(CONFIG_SCHED_INSTRUMENTATION_SWITCH)
#include <nuttx/note/note_driver.h> /* note_driver_register — the per-context-switch MPU-swap hook */
#endif
#include <fcntl.h> /* open — non-blocking console RX via NuttX's serial buffer */
#include <sched.h> /* task_delete */
#include <stdint.h>
#include <string.h>
#include <sys/random.h> /* getrandom — guest entropy (AT_RANDOM seed + getrandom(2)) */
#include <termios.h>	 /* tcgetattr/tcsetattr — put the console in raw mode (no NuttX echo/canon) */
#include <time.h>	 /* clock_gettime — PRNG fallback seed when no entropy source is configured */
#include <unistd.h>	 /* usleep, read */

#include "lxp/lxp_seam.h"
#include "ove/build.h"
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
#include "lxp/lxp_netfs.h" /* lxp_netfs_exec_stage — the remote-exec staging buffer */
#endif
#include "ove/time.h"	/* ove_time_get_us/ns -> engine time_us/time_ns ops */
#include "ove/thread.h" /* ove_thread_list -> engine thread_list op */

/* NuttX's own SVCall handler — chained (not patched) for non-Linux svcs.
 * Declared in arch/arm/src/common/arm_internal.h (off the app include path);
 * restated here as the one internal-symbol coupling. */
extern int arm_svcall(int irq, void *context, void *arg);
/* NuttX's HardFault handler (panics) — chained for a genuine kernel fault (same coupling pattern). */
extern int arm_hardfault(int irq, void *context, void *arg);

#define LXP_IRQ_SVCALL 11  /* == NuttX's internal NVIC_IRQ_SVCALL */
#define LXP_IRQ_MEMFAULT 4 /* == NuttX's internal NVIC_IRQ_MEMFAULT (MemManage) */
#define LXP_IRQ_BUSFAULT 5  /* == NuttX's NVIC_IRQ_BUSFAULT */
#define LXP_IRQ_USGFAULT 6  /* == NuttX's NVIC_IRQ_USAGEFAULT (undefined instr, bad control flow) */

/* ARMv7-M System Control Space (restated — the NuttX arch headers are off the app include path). */
#define OVE_SCS_SHCSR (*(volatile uint32_t *)0xE000ED24u) /* system handler ctrl/state */
#define OVE_SCS_CFSR (*(volatile uint32_t *)0xE000ED28u)  /* configurable fault status */
#define OVE_SHCSR_MEMFAULTENA (1u << 16)		  /* route MPU faults to MemManage (not HardFault) */
#define OVE_SHCSR_BUSFAULTENA (1u << 17)		  /* route bus faults to BusFault (not HardFault) */
#define OVE_SHCSR_USGFAULTENA (1u << 18)		  /* route usage faults to UsageFault (not HardFault) */
#define OVE_CFSR_MMFSR 0x000000ffu			  /* low byte = MemManage fault status (W1C) */

/* ARMv7-M MPU RASR SIZE for a power-of-2 region size. The region spans
 * 2^(FIELD+1) bytes, so FIELD = log2(size) - 1; RASR carries it at bits [5:1].
 * _FIELD gives the raw value (for code that shifts it itself), the other the
 * already-positioned one. */
#define OVE_MPU_RASR_SIZE_FIELD(sz) (30u - (unsigned)__builtin_clz((unsigned)(sz)))
#define OVE_MPU_RASR_SIZE(sz) (OVE_MPU_RASR_SIZE_FIELD(sz) << 1)

#define SLOT_PRIO 60 /* below the run-loop/main task (100) */

/* CONTROL.nPRIV — the unprivileged-thread-mode bit we OR into a program task's saved CONTROL so it
 * runs UNPRIVILEGED (restricted to the MPU regions). arch/arm/include/armv7-m/irq.h defines this and
 * REG_CONTROL (reached via <nuttx/irq.h>); restated as a fallback since it is otherwise off the app
 * include path. */
#ifndef CONTROL_NPRIV
#define CONTROL_NPRIV (1u << 0)
#endif

/* ---- NuttX-specific state -------------------------------------------------- */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Real STM32F746: the MCU's 320K internal SRAM (NuttX's heap) is far too small for the
 * multi-megabyte region pool, so it lives in the board's 8M external SDRAM (0xC0000000).
 * NuttX's CONFIG_STM32F7_FMC brings
 * the FMC + SDRAM up for its LTDC framebuffer (the first 255K at 0xC0000000) and uses none of the
 * rest as heap (the Linux config's three heap regions are all internal SRAM), so the span past
 * 1M is free. Fixed-address
 * pointers (NuttX owns its linker script — no NOLOAD section to hook). lxp_mpu_init() installs a
 * privileged-only, Normal non-cacheable base region over the whole SDRAM; the context-switch note
 * hook overlays the running program's exact data and dynamic-pool ranges as unprivileged RW. The
 * pool layout therefore keeps every per-program range power-of-2 sized and naturally aligned. */
#define NUTTX_SDRAM_POOL_BASE 0xC0100000u /* 1M past the SDRAM base, well clear of the framebuffer */
static uint8_t (*const dyn_pools)[LXP_DYN_POOL_SIZE] =
	(uint8_t(*)[LXP_DYN_POOL_SIZE])NUTTX_SDRAM_POOL_BASE;
static uint8_t (*const prog_regions)[LXP_PROG_REGION_SIZE] =
	(uint8_t(*)[LXP_PROG_REGION_SIZE])(NUTTX_SDRAM_POOL_BASE +
					    (size_t)LXP_NREG * LXP_DYN_POOL_SIZE);
_Static_assert((size_t)LXP_NREG * (LXP_DYN_POOL_SIZE + LXP_PROG_REGION_SIZE) <=
		       0xC0800000u - NUTTX_SDRAM_POOL_BASE,
	       "STM32 program pools overflow external SDRAM");
#elif defined(CONFIG_ARCH_BOARD_MPS2_AN500)
/* QEMU mps2-an500: the 16M block at 0x60000000 (QEMU mps.ram, fixed — the machine model rejects
 * any -m but 16). For unprivileged isolation the program pool must be a power-of-2-sized,
 * power-of-2-aligned block so ONE MPU region can grant it unprivileged-RW while everything else
 * stays kernel-only. Fixed pointers, like the STM32 branch — NuttX owns its linker script, so
 * there is no NOLOAD hook; a .bss array would straddle the kernel's own .bss at a non-power-of-2
 * boundary and defeat a clean per-region grant.
 *
 * The split, and why 8M is not negotiable:
 *
 *   [0x60000000, 0x60800000)   8M  rootfs.cpio XIP window
 *   [0x60800000, 0x61000000)   8M  program pool, granted unprivileged-RW
 *
 * The guest XIPs its FDPIC text straight out of the cpio, so the rootfs window needs its own
 * PMSAv7 region granting unprivileged RO+execute — and PMSAv7 regions are power-of-2 sized and
 * aligned. 8M is the largest window that leaves a non-overlapping pool above it: 16M would cover
 * the pool too and, being the higher-numbered region, would hand the guest RO+X over every
 * sibling's memory. A 12M window cannot be expressed at all. So the cpio must fit in 8M —
 * moving the pool up buys nothing.
 *
 * ove_config.cmake.j2 sizes LXP_NREG=5 for this pool: 5*256K program regions + 5*512K dynamic
 * pools = 3.75M. Its comment claims a "bottom 12 MiB" rootfs window, which is not achievable
 * for the reason above.
 *
 * Kernel RAM had to leave 0x60000000 regardless (see this board's nuttx/patches/0001-* and
 * ove_board_defconfig.linux): a rootfs at the base was overwritten by NuttX's .data copy and
 * .bss zeroing before it could be parsed. That move is what makes the full 8M usable. */
#define NUTTX_AN500_POOL_BASE 0x60800000u
_Static_assert((size_t)LXP_NREG * LXP_PROG_REGION_SIZE +
			       (size_t)LXP_NREG * LXP_DYN_POOL_SIZE <=
		       0x61000000u - NUTTX_AN500_POOL_BASE,
	       "an500 program pool overflows the top of mps.ram");
/* Put the larger-alignment array first. Every dynamic pool begins on its own
 * boundary, and the following program array is aligned whenever the aggregate
 * dynamic-pool extent is a multiple of the smaller program-region size. */
_Static_assert(((size_t)LXP_NREG * LXP_DYN_POOL_SIZE) % LXP_PROG_REGION_SIZE == 0,
	       "dynamic pool extent must align the following program regions");
static uint8_t (*const dyn_pools)[LXP_DYN_POOL_SIZE] =
	(uint8_t(*)[LXP_DYN_POOL_SIZE])NUTTX_AN500_POOL_BASE;
static uint8_t (*const prog_regions)[LXP_PROG_REGION_SIZE] =
	(uint8_t(*)[LXP_PROG_REGION_SIZE])(NUTTX_AN500_POOL_BASE +
					    (size_t)LXP_NREG * LXP_DYN_POOL_SIZE);
#else
static uint8_t prog_regions[LXP_NREG][LXP_PROG_REGION_SIZE] __attribute__((aligned(32)));
/* Per-region dynamic-link scratch pool: a dynamic FDPIC proc's arena lives here so ld.so can
 * mmap libc.so (~500K), far past the in-region arena. */
static uint8_t dyn_pools[LXP_NREG][LXP_DYN_POOL_SIZE] __attribute__((aligned(32)));
#endif
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* The framebuffer ends at 0xC003FC00. The next aligned span is privileged-only
 * cold coordinator storage, safely below the program pool at 0xC0100000. */
#define NUTTX_SDRAM_COLD_BASE 0xC0040000u
static lxp_exec_capture_t *const g_exec_captures =
	(lxp_exec_capture_t *)NUTTX_SDRAM_COLD_BASE;
_Static_assert(sizeof(lxp_exec_capture_t) * LXP_NSLOT <=
		       NUTTX_SDRAM_POOL_BASE - NUTTX_SDRAM_COLD_BASE,
	       "exec capture table overlaps the STM32 program pool");
#else
static lxp_exec_capture_t g_exec_captures[LXP_NSLOT];
#endif
/* Byte extents of the (contiguous) pools — sizeof() can't see through the STM32 fixed pointers. */
#define PROG_REGIONS_BYTES ((size_t)LXP_NREG * LXP_PROG_REGION_SIZE)
#define DYN_POOLS_BYTES ((size_t)LXP_NREG * LXP_DYN_POOL_SIZE)
static uintptr_t g_region_stack_lo[LXP_NREG];
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
/* Per-region flag: this region holds a program's OWN copied text (a remote exec off the 9P mount),
 * so region 2 must be mapped RWX (drop execute-never) instead of the default W^X. Indexed by region
 * index because set_prog_regions() — the note-driver switch hook — keys off ridx, not the slot. Set
 * at spawn from prog->region_exec; a normal (XIP-text) program leaves it 0, restoring W^X. */
static uint8_t g_region_exec[LXP_NREG];
#endif

#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
/* Remote-exec (9P netfs) staging buffer: the coordinator fetches a remote FDPIC ELF into
 * this 256K scratch, then launches it (its own text is copied into a program region). On the
 * SDRAM/PSRAM boards it sits immediately after the contiguous dyn+program pool window — still
 * inside the whole-pool Normal non-cacheable MPU region (region 1), so the privileged
 * coordinator reaches it (STM32: 0xC0700000..0xC0740000, well within the 8M SDRAM region).
 * Mirrors the FreeRTOS seam's g_netfs_exec_stage. */
#define NUTTX_EXEC_STAGE_BYTES (256u * 1024u)
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) || defined(CONFIG_ARCH_BOARD_MPS2_AN500)
static uint8_t *const g_netfs_exec_stage =
	(uint8_t *)((uintptr_t)prog_regions + PROG_REGIONS_BYTES);
#else
static uint8_t g_netfs_exec_stage[NUTTX_EXEC_STAGE_BYTES] __attribute__((aligned(32)));
#endif
uint8_t *lxp_netfs_exec_stage(size_t *cap)
{
	if (cap)
		*cap = NUTTX_EXEC_STAGE_BYTES;
	return g_netfs_exec_stage;
}
#endif /* CONFIG_OVE_LINUX_NETFS_EXEC */
static struct task_tcb_s g_tcb[LXP_NSLOT];
static int g_pid[LXP_NSLOT];

/* The RUNNING task. Our SVCall interposer and fault handlers ALWAYS run in exception context
 * (IPSR != 0). There, NuttX's nxsched_self()/this_task() == g_readytorun.head is the wrong
 * accessor: a nested IRQ (Ethernet RX making the HP work queue ready under net load) can splice
 * a higher-priority task to the ready-list head, so nxsched_self() returns THAT task — or a
 * transient garbage pointer mid-splice — instead of the guest that trapped, and the subsequent
 * ->pid deref BusFaults (intermittently, only under concurrent RX). The correct interrupt-context
 * accessor is g_running_tasks[cpu] (exactly what NuttX's own running_task() uses when
 * up_interrupt_context()). */
static inline struct tcb_s *lxp_running_tcb(void)
{
	return g_running_tasks[this_cpu()];
}

/* The slot whose task issued the svc (concurrent model: several may be live, so
 * match the running task's pid — NOT "the one used slot"). */
static int current_slot(void)
{
	pid_t self = lxp_running_tcb()->pid;
	for (int i = 0; i < LXP_NSLOT; i++)
		if (g_lxp_used[i] && g_pid[i] == self)
			return i;
	return -1;
}

/* The SVCall interposer. */
static int lxp_svc_handler(int irq, void *context, void *arg)
{
	uint32_t *regs = (uint32_t *)context;
	if (!g_lxp_active || !regs)
		return arm_svcall(irq, context, arg);
	/* Escalation gate + Linux-vs-NuttX discriminator. The Linux program runs UNPRIVILEGED, so a svc
	 * from an unprivileged frame (saved CONTROL.nPRIV set) is ALWAYS a Linux syscall — dispatch it
	 * here, NEVER chain to arm_svcall (which would let a hostile program invoke a NuttX scheduling
	 * svc; the program CAN reach a kernel svc site, the code region being unprivileged-RX). NuttX's
	 * own scheduling svcs come from privileged context → chain them. This supersedes the old
	 * PC-in-region test, which a program executing kernel .text could have slipped past. */
	if (!(regs[REG_CONTROL] & CONTROL_NPRIV))
		return arm_svcall(irq, context, arg);
	int sidx = current_slot();
	if (sidx < 0)
		return arm_svcall(irq, context, arg);

	/* arm_doirq() skips re-saving the interrupted context for an SVCall whose
	 * regs[REG_R0] == SYS_restore_context (== 1) — but a Linux syscall's r0 can
	 * legitimately be 1 (e.g. ioctl(fd=1, ...)), in which case arm_doirq would
	 * exception-return from a stale/NULL xcp.regs and crash. Re-assert the
	 * running task's saved-regs pointer so the return replays OUR frame. */
	lxp_running_tcb()->xcp.regs = regs;

	/* Populate the uniform frame, dispatch, write the modified HW regs back. */
	struct lxp_frame f;
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
#if LXP_ENABLE_FPU_CONTEXT
	/* Hard-float guest: carry its full VFP state through the dispatch so the shared core can
	 * preserve it across a parked/recreated syscall. CONFIG_ARCH_FPU makes NuttX save the whole
	 * s0-s31 + FPSCR into regs[] on exception entry (s0-s15/FPSCR in the HW frame, s16-s31 in the
	 * SW area), so this is a plain copy — no lazy-stack force like the FreeRTOS seam needs. */
	struct lxp_fp_context fpctx;
	f.fp = &fpctx;
	for (int i = 0; i < 16; i++)
		fpctx.s[i] = regs[REG_S0 + i];
	for (int i = 0; i < 16; i++)
		fpctx.s[16 + i] = regs[REG_S16 + i];
	fpctx.fpscr = regs[REG_FPSCR];
	fpctx.active = 1;
#endif

	lxp_dispatch(&f, &g_lxp_proc[sidx]);

	regs[REG_R0] = f.r[0];
	regs[REG_R1] = f.r[1];
	regs[REG_R2] = f.r[2];
	regs[REG_R3] = f.r[3];
	/* Write r4-r11 back too: a dispatch can rewrite a callee-saved register on the fast path
	 * (e.g. clone/vfork setting the child's frame, or a signal-return restoring context), and the
	 * guest must observe it across the SVC return. The exception-return restores regs[REG_R4..R11]
	 * into the guest's r4-r11, so mirror the (possibly modified) frame back here — the NuttX
	 * counterpart of the FreeRTOS seam's r4-r11 write-back (backends/freertos/freertos_lnx.c). */
	regs[REG_R4] = f.r[4];
	regs[REG_R5] = f.r[5];
	regs[REG_R6] = f.r[6];
	regs[REG_R7] = f.r[7];
	regs[REG_R8] = f.r[8];
	regs[REG_R9] = f.r[9];
	regs[REG_R10] = f.r[10];
	regs[REG_R11] = f.r[11];
	regs[REG_R12] = f.r[12];
	regs[REG_R14] = f.r[14];
	regs[REG_PC] = f.r[15];
	regs[REG_XPSR] = f.xpsr;
#if LXP_ENABLE_FPU_CONTEXT
	/* Write the (possibly dispatch-modified) VFP state back so the guest resumes with it. */
	for (int i = 0; i < 16; i++)
		regs[REG_S0 + i] = fpctx.s[i];
	for (int i = 0; i < 16; i++)
		regs[REG_S16 + i] = fpctx.s[16 + i];
	regs[REG_FPSCR] = fpctx.fpscr;
#endif
	return 0;
}

/* ---- the vtable: NuttX task spawn ------------------------------------------ */
static uint8_t *nuttx_region(int ridx)
{
	return prog_regions[ridx];
}

static uint8_t *nuttx_dyn_pool(int ridx, size_t *size)
{
	if (size)
		*size = LXP_DYN_POOL_SIZE;
	return dyn_pools[ridx];
}

static lxp_exec_capture_t *nuttx_exec_capture(int sidx)
{
	return (sidx >= 0 && sidx < LXP_NSLOT) ? &g_exec_captures[sidx] : NULL;
}

/* map_device (P3): install the framebuffer as an UNPRIVILEGED MPU region so a guest that
 * mmap'd /dev/fb0 (LV_LINUX_FBDEV_MMAP=1) writes pixels straight into it — no per-scanline
 * pwrite. NuttX uses only 5 of the M7's 8 regions (0,1 static + 2,3 per-program + 4 QSPI), so
 * this takes region 5: HIGHER than the priv-only whole-pool base (region 1), so it wins the
 * overlap and grants the guest unpriv RW to the fb while the rest of the pool stays priv-only.
 * set_prog_regions only ever rewrites regions 2+3, so region 5 survives every context switch.
 * size 0 tears it down (exec/relaunch). One display, shared by every program — not a memory-
 * safety concern (the segv/xregion isolation tests target kernel SRAM + sibling pools, not the
 * fb). Runs on the coordinator thread (raw MPU writes; NuttX FLAT leaves the MPU to us). */
#define LXP_FB_MPU_REGION 5u
static int nuttx_map_device(int sidx, uintptr_t addr, size_t size, unsigned attrs)
{
	(void)sidx;
	volatile uint32_t *const mpu_rnr = (uint32_t *)0xE000ED98u;
	volatile uint32_t *const mpu_rbar = (uint32_t *)0xE000ED9Cu;
	volatile uint32_t *const mpu_rasr = (uint32_t *)0xE000EDA0u;
	*mpu_rnr = LXP_FB_MPU_REGION;
	if (size == 0) {
		*mpu_rasr = 0; /* disable */
	} else {
		size_t rsz = 32u;
		while (rsz < size)
			rsz <<= 1; /* a PMSAv7 region size is a power of 2 */
		/* TEX/S/C/B from the LXP_MAP_* hint (ove/linux/dev.h): NC=0 -> 0x08 (the fb: the
		 * guest's stores reach SDRAM for the LTDC scanout), WT=1 -> 0x02, DEV=2 -> 0x01. */
		uint32_t texscb = (attrs == 1u) ? 0x02u : (attrs == 2u) ? 0x01u : 0x08u;
		*mpu_rbar = (uint32_t)(addr & ~(rsz - 1u));
		*mpu_rasr = (1u << 0) | OVE_MPU_RASR_SIZE(rsz) | (texscb << 16) | (0x3u << 24) |
			    (1u << 28); /* enable | size | attr | unpriv-RW | execute-never */
	}
	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
	return 0;
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

/* nxtask_init()/up_use_stack() colors (memsets STACK_COLOR over) the ENTIRE stack window on every
 * spawn. A program's window is the region tail — up to ~400K of uncached external SDRAM — so
 * coloring it on every resume cost ~2.7 ms/hop, the dominant NuttX multi-process latency (a pipe
 * round-trip = 2 hops ≈ 5.4 ms; a spawn = several). The color is purely a high-water-mark debug aid:
 * we override REG_SP with the real (captured/setup) SP right after, and the program's true stack
 * bound is its MPU region, so the colored span need not match the usable stack. Bound the coloring
 * to a small window just below the initial SP. Safe on our targets: CONFIG_ARMV7M_STACKCHECK is off
 * (nothing reads adj_stack_size at runtime) and ARMv7-M (Cortex-M7) has no PSPLIM. */
#define LXP_COLOR_WINDOW 0x2000u /* 8K — ample for nxtask_init's own frame setup */

/* Create slot `sidx` as a NuttX task. The usable stack is set by our REG_SP override; the window
 * passed here only governs the (bounded) coloring + the TCB stack top (= sp_top). */
static int spawn_task(int sidx, uintptr_t stack_lo, uintptr_t sp_top)
{
	uintptr_t color_lo = stack_lo;
	if (sp_top - stack_lo > LXP_COLOR_WINDOW)
		color_lo = sp_top - LXP_COLOR_WINDOW; /* color only the top window, not the whole region */
	memset(&g_tcb[sidx], 0, sizeof(g_tcb[sidx]));
	g_tcb[sidx].cmn.flags = TCB_FLAG_TTYPE_TASK; /* static TCB: no FREE_TCB/FREE_STACK */
	char nm[5] = {'l', 'n', 'x', (char)('0' + sidx), 0}; /* per-slot: ps/top per-proc CPU */
	if (nxtask_init(&g_tcb[sidx], nm, SLOT_PRIO, (void *)color_lo,
			(uint32_t)(sp_top - color_lo), slot_noentry, NULL, NULL, NULL) < 0)
		return -1;
	g_pid[sidx] = g_tcb[sidx].cmn.pid;
	g_lxp_used[sidx] = 1;
	return 0;
}

static int nuttx_spawn_launch(int sidx, int ridx, const lxp_flat_t *prog, void *entry, void *sp,
			      void *stack_lo)
{
	/* The prog/dyn regions are Normal WB-WA CACHEABLE (set_prog_regions), and regions are reused
	 * across execs. The loader just wrote THIS program's image to SDRAM non-cacheable (the region
	 * being loaded is never the currently-mapped one, so the coordinator's write falls through the
	 * non-cacheable base region 1), but stale cacheable lines from the PREVIOUS tenant of this ridx
	 * may still sit in the D-cache. Discard them so the program reads its freshly-loaded image, not
	 * the last tenant's cached data. A plain invalidate (no writeback) is correct: the discarded
	 * lines belong to an exited program, .bss/data were written straight to SDRAM by the loader, and
	 * the regions are 32-byte (cache-line) aligned. */
	up_invalidate_dcache((uintptr_t)prog_regions[ridx],
			     (uintptr_t)prog_regions[ridx] + LXP_PROG_REGION_SIZE);
	up_invalidate_dcache((uintptr_t)dyn_pools[ridx],
			     (uintptr_t)dyn_pools[ridx] + LXP_DYN_POOL_SIZE);
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	/* Record whether this program runs its own copied text from region 2 (a 9P-mount exec) so the
	 * note-driver switch hook maps region 2 RWX. lxp_note_resume reprograms when this changes even
	 * for a region already mapped (an execve reusing the same ridx flips a normal program to a
	 * remote-exec one). */
	if (ridx >= 0 && ridx < LXP_NREG)
		g_region_exec[ridx] = (uint8_t)prog->region_exec;
#endif
	g_region_stack_lo[ridx] = (uintptr_t)stack_lo;
	if (spawn_task(sidx, (uintptr_t)stack_lo, (uintptr_t)sp) != 0)
		return -1;
	uint32_t *regs = g_tcb[sidx].cmn.xcp.regs;
	regs[REG_PC] = (uint32_t)(uintptr_t)entry & ~1u;
	regs[REG_SP] = (uint32_t)(uintptr_t)sp;
	regs[REG_R0] = 0; /* static fini = NULL (uClinux entry convention) */
	/* FDPIC entry registers: r7 = the exec's loadmap
	 * (the crt _start self-relocates from it); r8 = ld.so's loadmap (dynamic only); r9 = the
	 * GOT base — for a dynamic exec ld.so's _start reads the entry r9 as its _DYNAMIC ptr. The
	 * resume path restores all three from the captured ctx->r4_11[3..5]. */
	regs[REG_R7] = prog->is_fdpic ? (uint32_t)prog->loadmap : 0u;
	regs[REG_R8] = prog->is_fdpic ? (uint32_t)prog->interp_loadmap : 0u;
	regs[REG_R9] = prog->is_fdpic ? (uint32_t)prog->got : 0u;
	regs[REG_CONTROL] |= CONTROL_NPRIV; /* run UNPRIVILEGED — MPU-restricted to its granted regions */
	nxtask_activate(&g_tcb[sidx].cmn);
	return 0;
}

static void nuttx_spawn_resume(int sidx, int ridx, const struct lxp_resume_ctx *ctx, long r0val)
{
	uintptr_t klo = g_region_stack_lo[ridx], ktop = (uintptr_t)ctx->sp;
	if (g_lxp_proc[sidx].is_thread) {
		/* Thread: run it on its clone child_stack (ctx->sp is the stack TOP, allocated by libpthread
		 * down in the region heap). nxtask_init([g_region_stack_lo, ctx->sp)) would INVERT (child_stack
		 * is below the main stack). A separate bookkeeping kstack fails too: NuttX sets PSP from the
		 * TCB stack top, ignoring our REG_SP override, so the thread would run on that tiny stack. Hand
		 * nxtask_init a small valid window at the TOP of the child stack instead → the TCB stack top IS
		 * child_stack, PSP lands there, and the thread runs on its full libpthread-allocated stack (no
		 * runtime stack-check; only the top 1 KB is colored). */
		klo = (uintptr_t)ctx->sp - 1024;
		ktop = (uintptr_t)ctx->sp;
	}
	if (spawn_task(sidx, klo, ktop) != 0)
		return;
	uint32_t *regs = g_tcb[sidx].cmn.xcp.regs;
	regs[REG_R4] = ctx->r4_11[0];
	regs[REG_R5] = ctx->r4_11[1];
	regs[REG_R6] = ctx->r4_11[2];
	regs[REG_R7] = ctx->r4_11[3];
	regs[REG_R8] = ctx->r4_11[4];
	regs[REG_R9] = ctx->r4_11[5];
	regs[REG_R10] = ctx->r4_11[6];
	regs[REG_R11] = ctx->r4_11[7];
	regs[REG_R12] = ctx->r12;
	regs[REG_R1] = ctx->r1;
	regs[REG_R2] = ctx->r2;
	regs[REG_R3] = ctx->r3;
	regs[REG_R14] = ctx->lr;
	regs[REG_SP] = ctx->sp;
	regs[REG_PC] = ctx->pc & ~1u;
	regs[REG_XPSR] = ctx->xpsr | (1u << 24); /* flags/IT state + Thumb */
	regs[REG_R0] = (uint32_t)r0val;
#if LXP_ENABLE_FPU_CONTEXT
	/* Restore the parked guest's VFP state into the recreated task's context. */
	if (ctx->fp.active) {
		for (int i = 0; i < 16; i++)
			regs[REG_S0 + i] = ctx->fp.s[i];
		for (int i = 0; i < 16; i++)
			regs[REG_S16 + i] = ctx->fp.s[16 + i];
		regs[REG_FPSCR] = ctx->fp.fpscr;
	}
#endif
	regs[REG_CONTROL] |= CONTROL_NPRIV; /* unprivileged — MPU-restricted (resumed vfork/clone child) */
	nxtask_activate(&g_tcb[sidx].cmn);
}

static void nuttx_abort_slot(int sidx)
{
	if (g_lxp_used[sidx] && g_pid[sidx] >= 0) {
		/* Force SYNCHRONOUS termination. The guest program runs on a stack window that
		 * OVERLAPS NuttX's per-task tls_info_s (TLS_INFO(sp) = sp masked to the stack base),
		 * so the program can clobber tl_cpstate. task_delete() -> nxnotify_cancellation()
		 * reads NONCANCELABLE from that field and, if the garbage happens to set it, DEFERS
		 * the delete (returns OK without terminating). But the parked guest spins in
		 * lxp_park_loop() — a tight for(;;) with no cancellation point — so it never reaches
		 * one and never dies; the very next spawn_task() then memset()s + nxtask_init()s this
		 * STILL-LIVE static g_tcb[] while it is on the ready-to-run list, corrupting the
		 * scheduler (garbage tcb -> BusFault under net load). TCB_FLAG_FORCED_CANCEL makes
		 * nxnotify_cancellation short-circuit before it dereferences the clobbered TLS, so the
		 * task is always terminated synchronously before its TCB is reused. */
		g_tcb[sidx].cmn.flags |= TCB_FLAG_FORCED_CANCEL;
		(void)task_delete(g_pid[sidx]);
	}
	g_lxp_used[sidx] = 0;
	g_pid[sidx] = -1;
}

static void nuttx_sleep_ms(unsigned ms)
{
	usleep(ms * 1000u);
}

/* Guest entropy source (lxp_os_ops_t.random_fill). WITHOUT this op the module's exec setup cannot
 * fill AT_RANDOM (the 16-byte stack-canary seed) and REFUSES to launch the process — so a missing
 * random_fill silently breaks every guest launch. getrandom() reads /dev/urandom, which the board
 * config backs with the STM32 hardware TRNG (CONFIG_STM32F7_RNG + CONFIG_DEV_URANDOM_ARCH), so this
 * normally returns real hardware entropy. The monotonic-clock-seeded xorshift below is a last-resort
 * fallback for the (now unexpected) case that getrandom() fails — kept so a transient RNG fault
 * cannot block every guest launch (AT_RANDOM tolerates a weak seed; the guest is MPU-isolated
 * regardless). Runs on the coordinator thread. */
static int nuttx_random_fill(void *buf, size_t len)
{
	if (getrandom(buf, len, 0) == (ssize_t)len)
		return 0;
	static uint32_t s;
	if (s == 0) {
		struct timespec ts = {0, 0};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		s = ((uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec << 16)) | 1u;
	}
	uint8_t *p = (uint8_t *)buf;
	for (size_t i = 0; i < len; i++) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		p[i] = (uint8_t)(s >> 24);
	}
	return 0;
}

/* Coordinator critical section: disable IRQs around the brief proc-table snapshot. */
static irqstate_t g_crit_flags;
static void nuttx_crit_enter(void)
{
	g_crit_flags = enter_critical_section();
}
static void nuttx_crit_exit(void)
{
	leave_critical_section(g_crit_flags);
}

/* Event wakeup: the coordinator blocks here; the dispatch (svc-interrupt context)
 * posts when a program parks. nxsem_post is interrupt-safe. */
static sem_t g_ev;
static void nuttx_event_post(void)
{
	nxsem_post(&g_ev);
}
static void nuttx_event_wait(unsigned ms)
{
	(void)nxsem_tickwait(&g_ev, MSEC2TICK(ms));
}

/* Bridge ove_thread_info -> the module-owned lxp_thread_info (identical layout) so
 * the seam can fill the engine's lxp_thread_info-typed thread_list op. */
static int lxp_seam_thread_list(struct lxp_thread_info *o, size_t m, size_t *n)
{
	return ove_thread_list((struct ove_thread_info *)o, m, n);
}

static int lxp_seam_mem_stats(struct lxp_mem_stats *out)
{
	struct ove_mem_stats m;
	int rc = ove_sys_get_mem_stats(&m);
	if (rc != OVE_OK)
		return rc;
	out->total = m.total;
	out->free = m.free;
	out->used = m.used;
	out->peak_used = m.peak_used;
	return LXP_OK;
}

#define LXP_SYSTEM_VERSION                                                    \
	"NuttX " CONFIG_VERSION_STRING " ove-" OVE_BUILD_OVERTOS_REV " lxp-" \
		OVE_BUILD_LXP_REV
_Static_assert(sizeof(LXP_SYSTEM_VERSION) <= 65u, "uname version exceeds Linux utsname field");
static const char *lxp_seam_system_version(void)
{
	return LXP_SYSTEM_VERSION;
}

/* Defined at end of file (they reference the MPU / IRQ helpers declared below);
 * the module's lxp_run() invokes them via g_lxp_host_engine.prepare/.teardown. */
static int nuttx_prepare(void);
static void nuttx_teardown(void);

const lxp_os_ops_t g_lxp_host_engine = {
	.prepare = nuttx_prepare,
	.teardown = nuttx_teardown,
	.region = nuttx_region,
	.dyn_pool = nuttx_dyn_pool,
	.exec_capture = nuttx_exec_capture,
	.map_device = nuttx_map_device,
	.spawn_launch = nuttx_spawn_launch,
	.spawn_resume = nuttx_spawn_resume,
	.abort_slot = nuttx_abort_slot,
	.sleep_ms = nuttx_sleep_ms,
	.crit_enter = nuttx_crit_enter,
	.crit_exit = nuttx_crit_exit,
	.event_post = nuttx_event_post,
	.event_wait = nuttx_event_wait,
	/* OS-service ops (host adapter). cache_* left NULL: NuttX's guest memory is
	 * coherent here (D-cache off), matching the former weak no-op lxp_guest_flush.
	 * coord_map left NULL too: the coordinator is privileged (PRIVDEFENA) with full
	 * access to the guest pools, so no per-slot cacheable MPU remap is needed. */
	.time_us = ove_time_get_us,
	.time_ns = ove_time_get_ns,
	.thread_list = lxp_seam_thread_list,
	.mem_stats = lxp_seam_mem_stats,
	.system_version = lxp_seam_system_version,
	.random_fill = nuttx_random_fill, /* REQUIRED: without it exec() can't seed AT_RANDOM → no launch */
};

/* ---- unprivileged isolation: MPU region setup ------------------------------ */
/* Run the Linux program UNPRIVILEGED behind a per-program MPU view so a stray/hostile access
 * faults MemManage (contained — see the memfault handler) instead of corrupting the kernel or a
 * sibling program. PRIVDEFENA keeps the privileged kernel on the ARM default map (unchanged); the
 * unprivileged program sees ONLY these regions:
 *   region 0 = code (flash/ROM): unprivileged RO + executable (XN=0) — the shared FDPIC text runs
 *              in-place from the embedded cpio here, and the contained-fault park loop lives here;
 *   region 1 = the program pool (PSRAM/SDRAM): unprivileged RW, execute-never (W^X).
 * Everything else — kernel .data/.bss/heap, peripherals — is ungranted, so an unprivileged access
 * to it faults. NuttX leaves the MPU disabled in FLAT, so we own it (raw registers; arm_mpu.c is
 * not compiled). */
static void lxp_mpu_init(void)
{
	volatile uint32_t *const mpu_ctrl = (uint32_t *)0xE000ED94u;
	volatile uint32_t *const mpu_rnr = (uint32_t *)0xE000ED98u;
	volatile uint32_t *const mpu_rbar = (uint32_t *)0xE000ED9Cu;
	volatile uint32_t *const mpu_rasr = (uint32_t *)0xE000EDA0u;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	const uint32_t code_base = 0x08000000u, code_sz = 19u; /* 1M internal flash */
	const uint32_t code_texscb = 0x02u;		       /* Normal write-through (real flash) */
	const uint32_t pool_base = 0xC0000000u, pool_sz = 22u; /* 8M external SDRAM (whole pool) */
#else					     /* QEMU mps2-an500 */
	const uint32_t code_base = 0x00000000u, code_sz = 20u; /* 2M ROM/flash at 0x0 (kernel .text) */
	const uint32_t code_texscb = 0x08u;		       /* Normal non-cacheable */
	/* Derived, not spelled again: this region must cover exactly the pool the
	 * prog_regions/dyn_pools pointers above are carved from. Hard-coding it a
	 * second time let the two drift — the base said 8M at 0x60800000 while the
	 * pool had moved to the top 4M, so the region granted the wrong span. RASR
	 * SIZE encodes 2^(SIZE+1) bytes: 4M -> 21. */
	const uint32_t pool_base = NUTTX_AN500_POOL_BASE;
	const uint32_t pool_sz = OVE_MPU_RASR_SIZE_FIELD(0x61000000u - NUTTX_AN500_POOL_BASE);
#endif
	/* Region 0: code (shared by every program) — priv RW / unpriv RO (AP=0b010), executable
	 * (XN=0). The FDPIC text runs in-place from the embedded cpio here + the park loop. The
	 * per-PROGRAM data regions (region 1 = prog_regions[ridx], region 2 = dyn_pools[ridx]) are
	 * (re)programmed on EVERY context switch by the note driver (set_prog_regions / lxp_note_
	 * resume), so a running program sees only ITS OWN region — not a sibling's. */
	*mpu_rnr = 0;
	*mpu_rbar = code_base;
	*mpu_rasr = (1u << 0) | (code_sz << 1) | (code_texscb << 16) | (0x2u << 24);
	/* Region 1: the WHOLE program pool, Normal non-cacheable, execute-never. In a fallback build with
	 * no context-switch hook it is unprivileged RW too (AP=0b011), providing kernel isolation but not
	 * sibling isolation. The normal per-switch configuration makes it PRIVILEGED-ONLY (AP=0b001) —
	 * the base that lets the privileged
	 * coordinator/seam touch ANY program's pool region as Normal memory; the per-program regions 2+3
	 * (set_prog_regions) grant the RUNNING program unprivileged RW to its OWN region, overriding this.
	 * Without the base, a non-running program's pool region falls to the ARM default map, which types
	 * the external SDRAM (0xC0000000) as DEVICE — and the coordinator's unaligned access to it
	 * Usage-Faults on the real M7 (QEMU's PSRAM default is Normal, so the an500 never hit it). */
	*mpu_rnr = 1;
	*mpu_rbar = pool_base;
#if defined(CONFIG_SCHED_INSTRUMENTATION_SWITCH)
	*mpu_rasr = (1u << 0) | (pool_sz << 1) | (0x08u << 16) | (0x1u << 24) | (1u << 28); /* priv RW, unpriv NO */
#else
	*mpu_rasr = (1u << 0) | (pool_sz << 1) | (0x08u << 16) | (0x3u << 24) | (1u << 28); /* fallback: RW/RW */
#endif
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	/* Region 4: the QSPI NOR XIP window. The UNPRIVILEGED guest XIPs its FDPIC text in place
	 * from 0x90000000 → unpriv RO + executable (XN=0), like the internal-flash code region 0.
	 * 16 MB (SIZE=23), 16 MB-aligned = one PMSAv7 region; Normal write-through so the I-cache
	 * absorbs the slow external-flash fetches (RO, so no coherence issue). STATIC and shared by
	 * every program — set_prog_regions only ever rewrites regions 2+3, so region 4 survives every
	 * context switch. Regions 0,1,4 static + 2,3 per-program = 5 of the M7's 8, no overlap. */
	*mpu_rnr = 4;
	*mpu_rbar = 0x90000000u;
	*mpu_rasr = (1u << 0) | (23u << 1) | (0x02u << 16) | (0x2u << 24);
#elif defined(CONFIG_ARCH_BOARD_MPS2_AN500)
	/* Region 4: the same XIP window for the an500, where the cpio is staged in the bottom of
	 * mps.ram by QEMU's -device loader rather than programmed into NOR. Without it the guest
	 * parses its rootfs and then cannot fetch a single instruction from it (MemManage IACCVIOL,
	 * CFSR 0x01): the ARM default map makes 0x60000000 unprivileged-inaccessible, and region 1
	 * covers only the pool above.
	 *
	 * 8 MB (SIZE=22) at 0x60000000, which is what forces the pool to 0x60800000: PMSAv7 regions
	 * are power-of-2 sized and aligned, and this must not overlap the pool — it is the
	 * higher-numbered region, so an overlap would override the pool's priv-only base and hand the
	 * guest RO+X over every sibling's memory. 16M would do exactly that; 12M cannot be expressed.
	 * So the rootfs.cpio has to fit in 8M. Normal non-cacheable (mps.ram is ordinary RAM in QEMU,
	 * unlike the STM32's NOR), RO so there is no coherence concern. */
	*mpu_rnr = 4;
	*mpu_rbar = 0x60000000u;
	*mpu_rasr = (1u << 0) | (OVE_MPU_RASR_SIZE_FIELD(NUTTX_AN500_POOL_BASE - 0x60000000u) << 1) |
		    (0x08u << 16) | (0x2u << 24);
#endif
	OVE_SCS_SHCSR |= OVE_SHCSR_MEMFAULTENA | OVE_SHCSR_BUSFAULTENA | OVE_SHCSR_USGFAULTENA; /* MPU faults → MemManage (contained), not HardFault */
	*mpu_ctrl = (1u << 0) | (1u << 2);	/* ENABLE | PRIVDEFENA */
	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
}

/* MemManage fault containment. The unprivileged program's stray/hostile access to an ungranted
 * address (kernel RAM, a peripheral, a sibling's region) traps HERE instead of corrupting the
 * system: kill JUST that program (exit 139 = 128+SIGSEGV), park it in the shared park loop, and wake
 * the coordinator so its EV_EXIT pass reaps the slot + reports the status to the parent (the shell
 * sees $?=139) — the kernel and any sibling programs run on. A MemManage from a non-program
 * (privileged) context is a genuine kernel bug → chain to NuttX's panicking HardFault path. */
static int lxp_memfault_handler(int irq, void *context, void *arg)
{
	uint32_t *regs = (uint32_t *)context;
	int sidx = (g_lxp_active && regs) ? current_slot() : -1;
	if (sidx < 0)
		return arm_hardfault(irq, context, arg); /* privileged kernel fault → NuttX panic */
	uint32_t cfsr = OVE_SCS_CFSR & 0x03ffffffu;
	uintptr_t fault_address = (cfsr & (1u << 7))	? *(volatile uint32_t *)0xE000ED34u
				  : (cfsr & (1u << 15)) ? *(volatile uint32_t *)0xE000ED38u
							: 0u;
	OVE_SCS_CFSR = cfsr; /* write-1-clear the set fault status (MM/Bus/Usage) */
	g_lxp_proc[sidx].exited = 1;
	g_lxp_proc[sidx].exit_status = 139; /* 128 + SIGSEGV */
	g_lxp_proc[sidx].exit_reason = LXP_EXIT_REASON_MEMORY_FAULT;
	g_lxp_proc[sidx].exit_signal = LXP_SIGSEGV;
	g_lxp_proc[sidx].exit_detail = cfsr;
	g_lxp_proc[sidx].exit_address = fault_address;
	regs[REG_PC] = (uint32_t)(uintptr_t)&lxp_park_loop & ~1u;
	regs[REG_XPSR] |= (1u << 24); /* keep Thumb state on exception return */
	lxp_event_post_slot(sidx); /* publish + wake coordinator → EV_EXIT reaps this slot */
	return 0;		      /* exception-return: the program spins in park_loop until reaped */
}

#if defined(CONFIG_SCHED_INSTRUMENTATION_SWITCH)
/* ---- inter-program isolation: per-program MPU regions on every context switch ---------------- */
/* Program regions 2+3 for the program that owns region `ridx`: region 2 = its data segment
 * (prog_regions[ridx]), region 3 = its dynamic-link arena (dyn_pools[ridx]) — HIGHER priority than
 * the privileged-only whole-pool base (region 1), so for THIS program those two ranges become
 * unprivileged RW while the rest of the pool stays privileged-only (a sibling's region is denied).
 * Both are power-of-2 sized and naturally aligned (the pool base is aligned to the region size and
 * the array stride equals the size), so each maps as one exact MPU region. Execute-never (W^X —
 * code lives in the shared region 0). */
/* Which region index is currently programmed into MPU regions 2+3. set_prog_regions is the ONLY
 * writer of those regions (lxp_mpu_init sets only 0+1), so this stays accurate — even across
 * lxp_run() calls, since a region index maps to a fixed memory range. -1 = none (boot). Lets
 * lxp_note_resume skip the 6 MPU writes + dsb + isb when the incoming program already owns the
 * mapped region (the common case: a syscall returns to the same program on every trap). */
static int g_mapped_ridx = -1;
static int g_mapped_exec = -1; /* exec-ness (g_region_exec) of the region currently mapped into region 2 */

static void set_prog_regions(int ridx)
{
	volatile uint32_t *const mpu_rnr = (uint32_t *)0xE000ED98u;
	volatile uint32_t *const mpu_rbar = (uint32_t *)0xE000ED9Cu;
	volatile uint32_t *const mpu_rasr = (uint32_t *)0xE000EDA0u;
	/* TEX=001,C=1,B=1 = Normal Write-Back Write-Allocate CACHEABLE (was 0x08 = non-cacheable). The
	 * program's data/bss/heap/stack live in region 2 and ld.so's arena in region 3; LVGL's malloc'd
	 * draw buffer lands here, so caching the per-pixel compositing writes is a large win on the
	 * memory-bound heavy scenes (text/containers/layers), which were writing every pixel straight to
	 * uncached FMC SDRAM. Safe: no DMA reads these regions — the LTDC scans only the framebuffer
	 * @0xC0000000, covered by the non-cacheable base region 1 — and the privileged personality reads
	 * the draw buffer coherently on the same core, then blits to the non-cacheable framebuffer, so no
	 * SCB cache maintenance is needed. (No code executes from here: FDPIC text XIPs from QSPI/reg 4.) */
	uint32_t reg2_xn = (1u << 28); /* execute-never (W^X): FDPIC text XIPs from QSPI/region 4 */
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	/* A remote-exec proc runs its OWN text copied into region 2 → map it RWX (drop XN). A
	 * per-process, MPU-contained W^X relaxation, only for a program launched off the remote mount. */
	if (ridx >= 0 && ridx < LXP_NREG && g_region_exec[ridx])
		reg2_xn = 0u;
#endif
	*mpu_rnr = 2;
	*mpu_rbar = (uint32_t)(uintptr_t)prog_regions[ridx];
	*mpu_rasr = (1u << 0) | OVE_MPU_RASR_SIZE(LXP_PROG_REGION_SIZE) | (0x0Bu << 16) |
		    (0x3u << 24) | reg2_xn;
	*mpu_rnr = 3;
	*mpu_rbar = (uint32_t)(uintptr_t)dyn_pools[ridx];
	*mpu_rasr = (1u << 0) | OVE_MPU_RASR_SIZE(LXP_DYN_POOL_SIZE) | (0x0Bu << 16) | (0x3u << 24) |
		    (1u << 28);
	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
	g_mapped_ridx = ridx;
	g_mapped_exec = (reg2_xn == 0u); /* record region 2's exec-ness so a same-ridx flip reprograms */
}

/* Note-driver resume hook — fires on EVERY switch TO a task (sched_note_resume, in
 * sched_switchcontext), INCLUDING round-robin preemption between two runnable program tasks that
 * never enters the seam's svc handler. If the incoming task is a program slot, swap the MPU to ITS
 * region so it cannot reach a sibling's. Kernel/coordinator tasks are privileged (PRIVDEFENA), so
 * their region set is irrelevant → skip (leaving the last program's regions is harmless; privileged
 * access uses the default map). Runs in the switch context: a few register writes + a barrier, no
 * allocation, no blocking. */
static void lxp_note_resume(struct note_driver_s *drv, struct tcb_s *tcb)
{
	(void)drv;
	if (!g_lxp_active || !tcb)
		return;
	/* Defensive: this hook fires on EVERY switch, including kernel/coordinator tasks. A valid
	 * tcb lives in on-chip SRAM/DTCM (0x2000_0000..0x2008_0000); anything else would BusFault on
	 * the tcb->pid deref (and no program slot could match a non-RAM pid holder anyway). */
	if ((uintptr_t)tcb < 0x20000000u || (uintptr_t)tcb >= 0x20080000u)
		return;
	pid_t pid = tcb->pid;
	for (int i = 0; i < LXP_NSLOT; i++) {
		if (g_lxp_used[i] && g_pid[i] == pid) {
			int ridx = g_lxp_proc[i].region;
			int want_exec = 0;
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
			if (ridx >= 0 && ridx < LXP_NREG)
				want_exec = g_region_exec[ridx];
#endif
			/* Reprogram when the region changed OR its exec-ness flipped (an execve reused this
			 * ridx to launch a remote-exec program needing RWX); else skip the 6 MPU writes. */
			if (ridx != g_mapped_ridx || want_exec != g_mapped_exec)
				set_prog_regions(ridx);
			return;
		}
	}
}

static const struct note_driver_ops_s g_lxp_note_ops = {
	.resume = lxp_note_resume,
};
static struct note_driver_s g_lxp_note_driver = {
	.ops = &g_lxp_note_ops,
};
#endif /* CONFIG_SCHED_INSTRUMENTATION_SWITCH */

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* The Linux personality console (apps/.../app.c, the CONFIG_OVE_BOARD_STM32F746G_DISCO branch) polls
 * USART1 directly. NuttX already brought USART1 up as its own console (CONFIG_USART1), so we only
 * steal RX from its IRQ path: serial_poll_begin() clears RXNEIE so the personality's polled reads own
 * the receiver (raw register polling works in the svc-exception context, where NuttX's IRQ-driven
 * serial read would deadlock). STM32F7 USART1 @ 0x40011000: CR1(0x00) RXNEIE=b5, ISR(0x1C) RXNE=b5
 * TXE=b7, RDR(0x24), TDR(0x28). */
#define OVE_NX_USART1 0x40011000u
#define OVE_NX_U1_CR1 (*(volatile uint32_t *)(OVE_NX_USART1 + 0x00u))
#define OVE_NX_U1_ISR (*(volatile uint32_t *)(OVE_NX_USART1 + 0x1Cu))
#define OVE_NX_U1_RDR (*(volatile uint32_t *)(OVE_NX_USART1 + 0x24u))
#define OVE_NX_U1_TDR (*(volatile uint32_t *)(OVE_NX_USART1 + 0x28u))
/* RX rides NuttX's own IRQ-filled serial receive buffer, read non-blocking through the console
 * device — NOT a raw poll of the 1-byte RDR. NuttX owns the USART1 RX IRQ (RXNEIE) and drains each
 * arriving byte into its recv FIFO; a raw RDR poll both races that IRQ (bytes vanish into NuttX's
 * buffer) and overruns the single RDR on a multi-byte paste. Reading the FIFO instead captures every
 * byte, mirroring the FreeRTOS serial_wrapper.c "read the IRQ buffer, not the RDR" design. The
 * personality's console read is parked and resumed from the run-loop (task) context, so the
 * non-blocking read() is a normal file op there; O_NONBLOCK also means it never waits. TX stays a
 * direct polled TDR write below (the personality owns TX once it starts; NuttX does no console TX
 * after boot). */
static int g_con_rfd = -1;
static int g_rx_look = -1; /* one-byte lookahead: rx_ready pulls from the FIFO, getc returns it */
void serial_poll_begin(void)
{
	if (g_con_rfd >= 0)
		return;
	g_con_rfd = open("/dev/console", O_RDONLY | O_NONBLOCK);
#if defined(CONFIG_SERIAL_TERMIOS)
	/* Raw console: the personality's guest shell owns echo + line editing, so strip NuttX's
	 * default cooked mode (ISIG|ECHO|ICANON). Without this NuttX echoes every keystroke a second
	 * time (doubled input) and line-buffers RX to a newline instead of delivering bytes as they
	 * arrive. VMIN=0/VTIME=0 keeps read() non-blocking. */
	if (g_con_rfd >= 0) {
		struct termios t;
		if (tcgetattr(g_con_rfd, &t) == 0) {
			t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
			t.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON);
			t.c_cc[VMIN] = 0;
			t.c_cc[VTIME] = 0;
			tcsetattr(g_con_rfd, TCSANOW, &t);
		}
	}
#endif
}
int serial_poll_rx_ready(void)
{
	if (g_rx_look >= 0)
		return 1;
	unsigned char c;
	if (g_con_rfd >= 0 && read(g_con_rfd, &c, 1) == 1) {
		g_rx_look = (int)c;
		return 1;
	}
	return 0;
}
int serial_poll_getc(void)
{
	if (g_rx_look >= 0) {
		int c = g_rx_look;
		g_rx_look = -1;
		return c;
	}
	unsigned char c = 0;
	if (g_con_rfd >= 0 && read(g_con_rfd, &c, 1) == 1)
		return (int)c;
	return 0;
}
void serial_poll_putc(char c)
{
	while (!(OVE_NX_U1_ISR & (1u << 7))) { /* wait for TXE */
	}
	OVE_NX_U1_TDR = (unsigned char)c;
}
#endif

/* Per-run bring-up / teardown (was the body of the old lxp_run() wrapper). The
 * public lxp_run() now lives in the module (src/lxp_run.c) and calls these via
 * g_lxp_host_engine.prepare()/.teardown() around lxp_run_common(). */
static int nuttx_prepare(void)
{
	lxp_mpu_init(); /* unprivileged-isolation regions + enable the MPU (both boards) */
	for (int i = 0; i < LXP_NSLOT; i++)
		g_pid[i] = -1;
	nxsem_init(&g_ev, 0, 0); /* coordinator wakeup sem */
	irq_attach(LXP_IRQ_SVCALL, lxp_svc_handler, NULL);
	irq_attach(LXP_IRQ_MEMFAULT, lxp_memfault_handler, NULL); /* contain program MPU faults */
	irq_attach(LXP_IRQ_BUSFAULT, lxp_memfault_handler, NULL); /* + bus faults */
	irq_attach(LXP_IRQ_USGFAULT, lxp_memfault_handler, NULL); /* + usage faults (bad instr) */
#if defined(CONFIG_SCHED_INSTRUMENTATION_SWITCH)
	note_driver_register(&g_lxp_note_driver); /* per-switch per-program MPU region swap */
#endif
	return 0;
}

static void nuttx_teardown(void)
{
	irq_attach(LXP_IRQ_SVCALL, arm_svcall, NULL);	 /* restore NuttX's handlers */
	irq_attach(LXP_IRQ_MEMFAULT, arm_hardfault, NULL);
	irq_attach(LXP_IRQ_BUSFAULT, arm_hardfault, NULL);
	irq_attach(LXP_IRQ_USGFAULT, arm_hardfault, NULL);
}

#endif /* CONFIG_OVE_LINUX */
