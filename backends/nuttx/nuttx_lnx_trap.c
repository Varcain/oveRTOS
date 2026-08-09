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
 * Each program runs as a real NuttX task created with nxtask_init(). NuttX's
 * registered stack and TLS live in privileged seam-owned memory; only the ARM
 * exception frame and live PSP are placed on the guest stack. The initial
 * register context is set to the uClinux entry state (resume replays the
 * captured vfork context).
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

#include <nuttx/config.h>

#if !defined(CONFIG_BUILD_FLAT)
#error "The NuttX Linux personality requires CONFIG_BUILD_FLAT"
#endif
#if defined(CONFIG_ARM_MPU)
#error "The NuttX Linux personality owns the MPU; CONFIG_ARM_MPU must be disabled"
#endif
#if !defined(CONFIG_DRIVERS_NOTE) || !defined(CONFIG_SCHED_INSTRUMENTATION_SWITCH)
#error "The NuttX Linux personality requires the scheduler-switch note hook"
#endif

#include <stddef.h>

#include <nuttx/arch.h>	     /* up_perf_gettime — exact guest runtime accounting */
#include <nuttx/clock.h>     /* MSEC2TICK */
#include <nuttx/irq.h>	     /* irq_attach, enter/leave_critical_section; arch/irq.h REG_* */
#include <nuttx/queue.h>     /* dq_rem — move a parked TCB out of the stopped list */
#include <nuttx/sched.h>     /* nxtask_init, nxtask_activate, struct task_tcb_s */
#include <nuttx/semaphore.h> /* nxsem_init/post/tickwait — coordinator wakeup */
#include <nuttx/version.h>
#include <nuttx/note/note_driver.h> /* note_driver_register — the per-context-switch MPU-swap hook */
#include <errno.h>
#include <sched.h> /* task_delete */
#include <stdint.h>
#include <string.h>
#include <sys/random.h> /* getrandom — guest entropy (AT_RANDOM seed + getrandom(2)) */

#include "lxp/lxp_exec.h"
#include "lxp/lxp_run.h"
#include "lxp/lxp_seam.h"
#include "ove/build.h"
#include "ove/lxp_memory_layout.h"
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
#include "ove/lxp_metrics.h"
#endif
#include "ove/time.h"	/* ove_time_get_us/ns -> engine time_us/time_ns ops */
#include "ove/thread.h" /* ove_thread_list -> engine thread_list op */
#include "lxp_ove_thread_adapter.h"
#include "ove_cortex_m_cache.h"
#include "ove_cortex_m_mpu.h"
#include "ove_lxp_memory_contract.h"
#include "ove_nuttx_runtime.h"

/* NuttX's own SVCall handler — chained (not patched) for non-Linux svcs.
 * Declared in arch/arm/src/common/arm_internal.h (off the app include path);
 * restated here as the one internal-symbol coupling. */
extern int arm_svcall(int irq, void *context, void *arg);
/* NuttX's HardFault handler (panics) — chained for a genuine kernel fault (same coupling pattern). */
extern int arm_hardfault(int irq, void *context, void *arg);
/* Scheduler-internal stopped-list primitive, built by CONFIG_SIG_SIGSTOP_ACTION.
 * Its declaration lives in sched/sched/sched.h (outside the app include path). */
extern void nxsched_suspend(struct tcb_s *tcb);
extern bool nxsched_add_readytorun(struct tcb_s *tcb);
extern dq_queue_t g_stoppedtasks;

#define LXP_IRQ_SVCALL 11  /* == NuttX's internal NVIC_IRQ_SVCALL */
#define LXP_IRQ_MEMFAULT 4 /* == NuttX's internal NVIC_IRQ_MEMFAULT (MemManage) */
#define LXP_IRQ_BUSFAULT 5 /* == NuttX's NVIC_IRQ_BUSFAULT */
#define LXP_IRQ_USGFAULT 6 /* == NuttX's NVIC_IRQ_USAGEFAULT (undefined instr, bad control flow) */

/* ARMv7-M System Control Space (restated — the NuttX arch headers are off the app include path). */
#define OVE_SCS_SHCSR (*(volatile uint32_t *)0xE000ED24u) /* system handler ctrl/state */
#define OVE_SCS_CFSR (*(volatile uint32_t *)0xE000ED28u)  /* configurable fault status */
#define OVE_SHCSR_MEMFAULTENA (1u << 16) /* route MPU faults to MemManage (not HardFault) */
#define OVE_SHCSR_BUSFAULTENA (1u << 17) /* route bus faults to BusFault (not HardFault) */
#define OVE_SHCSR_USGFAULTENA (1u << 18) /* route usage faults to UsageFault (not HardFault) */
#define OVE_CFSR_MMFSR 0x000000ffu	 /* low byte = MemManage fault status (W1C) */

/* ARMv7-M MPU RASR SIZE for a power-of-2 region size. The region spans
 * 2^(FIELD+1) bytes, so FIELD = log2(size) - 1; RASR carries it at bits [5:1].
 * _FIELD gives the raw value (for code that shifts it itself), the other the
 * already-positioned one. */
#define OVE_MPU_RASR_SIZE_FIELD(sz) (30u - (unsigned)__builtin_clz((unsigned)(sz)))
#define OVE_MPU_RASR_SIZE(sz) (OVE_MPU_RASR_SIZE_FIELD(sz) << 1)

#define SLOT_PRIO 60 /* below the run-loop/main task (100) */

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
uint32_t ove_lxp_metrics_counter_hz(void)
{
	return (uint32_t)up_perf_getfreq();
}
#endif

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Coordinator base and guest overlays use this identical Normal-memory type. */
#define NUTTX_POOL_TEXSCB 0x0Bu /* WBWA, non-shareable */
#else
#define NUTTX_POOL_TEXSCB 0x08u /* Normal non-cacheable */
#endif

/* CONTROL.nPRIV — the unprivileged-thread-mode bit we OR into a program task's saved CONTROL so it
 * runs UNPRIVILEGED (restricted to the MPU regions). arch/arm/include/armv7-m/irq.h defines this and
 * REG_CONTROL (reached via <nuttx/irq.h>); restated as a fallback since it is otherwise off the app
 * include path. */
#ifndef CONTROL_NPRIV
#define CONTROL_NPRIV (1u << 0)
#endif

/* NuttX's armv7-m/exc_return.h is internal to the kernel build. Reproduce the
 * thread-return value used by up_initial_state(): PSP when an IRQ stack is
 * configured, plus an extended hardware frame when the FPU is enabled. */
#define LXP_EXC_RETURN_BASE 0xffffffe1u
#if CONFIG_ARCH_INTERRUPTSTACK > 7
#define LXP_EXC_RETURN_STACK (1u << 2)
#else
#define LXP_EXC_RETURN_STACK 0u
#endif
#ifdef CONFIG_ARCH_FPU
#define LXP_EXC_RETURN_FPU 0u
#else
#define LXP_EXC_RETURN_FPU (1u << 4)
#endif
#define LXP_EXC_RETURN_THREAD \
	(LXP_EXC_RETURN_BASE | LXP_EXC_RETURN_STACK | LXP_EXC_RETURN_FPU | (1u << 3))

/* ---- NuttX-specific state -------------------------------------------------- */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Real STM32F746: the MCU's 320K internal SRAM (NuttX's heap) is far too small for the
 * multi-megabyte region pool, so it lives in the board's 8M external SDRAM (0xC0000000).
 * NuttX's CONFIG_STM32F7_FMC brings
 * the FMC + SDRAM up for its LTDC framebuffer (the first 255K at 0xC0000000) and uses none of the
 * rest as heap (the Linux config's three heap regions are all internal SRAM), so the span past
 * 1M is free. Fixed-address
 * pointers (NuttX owns its linker script — no NOLOAD section to hook). lxp_mpu_init() installs a
 * privileged-only cacheable Normal-memory base over the program pool, matching
 * the guest overlays; its first 1M subregion stays disabled for the LTDC
 * framebuffer. The context-switch note hook overlays the running program's
 * exact data and dynamic-pool ranges as unprivileged RW. */
static uint8_t (*const dyn_pools)[LXP_DYN_POOL_SIZE] = (uint8_t (*)[LXP_DYN_POOL_SIZE])
	OVE_LXP_GUEST_POOL_BASE;
static uint8_t (*const prog_regions)[LXP_PROG_REGION_SIZE] = (uint8_t (*)[LXP_PROG_REGION_SIZE])(
	OVE_LXP_GUEST_POOL_BASE + (size_t)LXP_NREG * LXP_DYN_POOL_SIZE);
_Static_assert((size_t)LXP_NREG *(LXP_DYN_POOL_SIZE + LXP_PROG_REGION_SIZE) <=
		       OVE_LXP_GUEST_POOL_SIZE,
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
 * ove_config.cmake.j2 sizes LXP_NREG=6 for this pool: 6*256K program regions + 6*512K dynamic
 * pools = 4.5M. The remaining capacity is deliberate headroom for future bounded row growth.
 *
 * Kernel RAM had to leave 0x60000000 regardless (see this board's nuttx/patches/0001-* and
 * ove_board_defconfig.linux): a rootfs at the base was overwritten by NuttX's .data copy and
 * .bss zeroing before it could be parsed. That move is what makes the full 8M usable. */
_Static_assert(OVE_LXP_ROOTFS_END == OVE_LXP_GUEST_POOL_BASE,
	       "AN500 rootfs and guest-pool ranges must be adjacent");
_Static_assert((size_t)LXP_NREG *LXP_PROG_REGION_SIZE + (size_t)LXP_NREG * LXP_DYN_POOL_SIZE <=
		       OVE_LXP_GUEST_POOL_SIZE,
	       "an500 program pool overflows the top of mps.ram");
/* Put the larger-alignment array first. Every dynamic pool begins on its own
 * boundary, and the following program array is aligned whenever the aggregate
 * dynamic-pool extent is a multiple of the smaller program-region size. */
_Static_assert(((size_t)LXP_NREG * LXP_DYN_POOL_SIZE) % LXP_PROG_REGION_SIZE == 0,
	       "dynamic pool extent must align the following program regions");
static uint8_t (*const dyn_pools)[LXP_DYN_POOL_SIZE] = (uint8_t (*)[LXP_DYN_POOL_SIZE])
	OVE_LXP_GUEST_POOL_BASE;
static uint8_t (*const prog_regions)[LXP_PROG_REGION_SIZE] = (uint8_t (*)[LXP_PROG_REGION_SIZE])(
	OVE_LXP_GUEST_POOL_BASE + (size_t)LXP_NREG * LXP_DYN_POOL_SIZE);
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
static lxp_exec_capture_t *const g_exec_captures = (lxp_exec_capture_t *)NUTTX_SDRAM_COLD_BASE;
#define NUTTX_SDRAM_THREAD_SNAPSHOT_BASE \
	LXP_ALIGN_UP(NUTTX_SDRAM_COLD_BASE + sizeof(lxp_exec_capture_t) * LXP_NSLOT, 8u)
#define g_thread_snapshot (*(struct lxp_ove_thread_snapshot *)NUTTX_SDRAM_THREAD_SNAPSHOT_BASE)
#else
static lxp_exec_capture_t g_exec_captures[LXP_NSLOT];
static struct lxp_ove_thread_snapshot g_thread_snapshot;
#endif

/* nxtask_init stores struct tls_info_s at stack_alloc_ptr and NuttX later
 * trusts its cancellation, cleanup, errno, and task-info fields. Keep that
 * allocation outside every unprivileged guest MPU range. The task never
 * executes ordinary code on this substrate stack: spawn_task() relocates the
 * initial ARM exception frame to the guest PSP before activation. 1 KiB is
 * already the proven minimum used by the former clone-thread path and leaves
 * ample room for TLS, argv metadata, and the full FPU exception frame. */
#define LXP_NUTTX_STACK_SIZE 1024u
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#define LXP_ALIGN_UP(value, align) (((value) + (align) - 1u) & ~((align) - 1u))
#define NUTTX_SDRAM_STACK_BASE \
	LXP_ALIGN_UP(NUTTX_SDRAM_THREAD_SNAPSHOT_BASE + sizeof(g_thread_snapshot), 8u)
static uint8_t (*const g_nuttx_stacks)[LXP_NUTTX_STACK_SIZE] = (uint8_t (*)[LXP_NUTTX_STACK_SIZE])
	NUTTX_SDRAM_STACK_BASE;
_Static_assert(NUTTX_SDRAM_STACK_BASE + LXP_NUTTX_STACK_SIZE * LXP_NSLOT <= OVE_LXP_GUEST_POOL_BASE,
	       "trusted NuttX slot stacks overlap the STM32 program pool");
#else
static uint8_t g_nuttx_stacks[LXP_NSLOT][LXP_NUTTX_STACK_SIZE] __attribute__((aligned(8)));
#endif

/* Byte extents of the (contiguous) pools — sizeof() can't see through the STM32 fixed pointers. */
#define PROG_REGIONS_BYTES ((size_t)LXP_NREG * LXP_PROG_REGION_SIZE)
#define DYN_POOLS_BYTES ((size_t)LXP_NREG * LXP_DYN_POOL_SIZE)
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
#define NUTTX_EXEC_STAGE_BYTES (256u * 1024u)
#else
#define NUTTX_EXEC_STAGE_BYTES 0u
#endif
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && defined(LXP_WFS_POOL_BASE)
#define NUTTX_GUEST_STORAGE_END \
	(OVE_LXP_GUEST_POOL_BASE + DYN_POOLS_BYTES + PROG_REGIONS_BYTES + NUTTX_EXEC_STAGE_BYTES)
_Static_assert(NUTTX_GUEST_STORAGE_END <= (uintptr_t)LXP_WFS_POOL_BASE,
	       "NuttX guest storage overlaps the fixed tmpfs pool");
_Static_assert((uintptr_t)LXP_WFS_POOL_BASE + (size_t)LXP_WFS_POOL <= OVE_LXP_GUEST_POOL_END,
	       "NuttX tmpfs pool exceeds external SDRAM");
#endif
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
/* Remote-exec (9P netfs) staging buffer: the coordinator fetches a remote FDPIC ELF into
 * this 256K scratch, then launches it (its own text is copied into a program region). On the
 * SDRAM/PSRAM boards it sits immediately after the contiguous dyn+program pool window — still
 * inside the whole-pool privileged Normal-memory MPU region (STM32 WBWA, QEMU
 * non-cacheable), so the coordinator reaches it (STM32:
 * 0xC0700000..0xC0740000, well within the 8M SDRAM region).
 * Mirrors the FreeRTOS seam's g_netfs_exec_stage. */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) || defined(CONFIG_ARCH_BOARD_MPS2_AN500)
static uint8_t *const g_netfs_exec_stage =
	(uint8_t *)((uintptr_t)prog_regions + PROG_REGIONS_BYTES);
#else
static uint8_t g_netfs_exec_stage[NUTTX_EXEC_STAGE_BYTES] __attribute__((aligned(32)));
#endif
static uint8_t *nuttx_exec_stage(size_t *cap)
{
	if (cap)
		*cap = NUTTX_EXEC_STAGE_BYTES;
	return g_netfs_exec_stage;
}
#endif /* CONFIG_OVE_LINUX_NETFS_EXEC */
static struct task_tcb_s g_tcb[LXP_NSLOT];
static pid_t g_guest_budget_pid = -1;

/* Device mappings are part of a Linux slot's unprivileged MPU view, not global
 * process state. Regions 5 and 6 cover the two ranges recorded by
 * lxp_proc_t::dev_map_{lo,hi}; regions 0,1,4 are static and 2,3 select the
 * incoming program's ordinary memory. Store the already encoded RBAR/RASR so
 * the context-switch hook performs fixed work without recomputing a region. */
#define LXP_DEVICE_MPU_FIRST 5u
#define LXP_DEVICE_MPU_COUNT 2u
struct nuttx_device_map {
	uintptr_t addr;
	size_t size;
	uint32_t rbar;
	uint32_t rasr;
	uint8_t attrs;
	uint8_t used;
};

#define LXP_NATIVE_POLICY_REGIONS (3u + LXP_DEVICE_MPU_COUNT)
struct nuttx_prepared_profile {
	lxp_memory_policy_key_t key;
	uint32_t rbar[LXP_NATIVE_POLICY_REGIONS];
	uint32_t rasr[LXP_NATIVE_POLICY_REGIONS];
	uint8_t valid;
};
struct nuttx_lxp_slot {
	int pid;
	uint32_t generation;
	struct nuttx_device_map device_maps[LXP_DEVICE_MPU_COUNT];
	struct nuttx_prepared_profile profile;
};
static struct nuttx_lxp_slot g_slots[LXP_NSLOT];
/* NuttX owns the opaque task control blocks and stacks; the seam owns g_slots. */
static lxp_memory_policy_key_t g_installed_policy;
static uint8_t g_installed_policy_valid;
static void nuttx_park_entry(void *token);
static int nuttx_profile_is_current(int sidx);

static lxp_slot_ref_t task_slot_ref(int slot)
{
	return (lxp_slot_ref_t){
		.index = (int16_t)slot,
		.generation = slot >= 0 && slot < LXP_NSLOT ? g_slots[slot].generation : 0,
	};
}

static uint64_t guest_runtime_us(int sidx)
{
	uint64_t cycles = 0;
	if (sidx >= 0 && sidx < LXP_NSLOT && g_slots[sidx].pid >= 0)
		(void)ove_nuttx_runtime_get(g_slots[sidx].pid, &cycles, NULL);
	return ove_nuttx_runtime_cycles_to_us(cycles);
}

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
		if (g_slots[i].pid == self && lxp_slot_ref_is_runnable(task_slot_ref(i)))
			return i;
	return -1;
}

/* The SVCall interposer. */
static int lxp_svc_handler(int irq, void *context, void *arg)
{
	uint32_t *regs = (uint32_t *)context;
	if (!lxp_trap_active() || !regs)
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

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	uint32_t svc_syscall = regs[REG_R7];
	uint32_t svc_start_cycles = (uint32_t)up_perf_gettime();
#endif

	/* arm_doirq() skips re-saving the interrupted context for an SVCall whose
	 * regs[REG_R0] == SYS_restore_context (== 1) — but a Linux syscall's r0 can
	 * legitimately be 1 (e.g. ioctl(fd=1, ...)), in which case arm_doirq would
	 * exception-return from a stale/NULL xcp.regs and crash. Re-assert the
	 * running task's saved-regs pointer so the return replays OUR frame. */
	lxp_running_tcb()->xcp.regs = regs;
	if (!nuttx_profile_is_current(sidx)) {
		lxp_guest_fault_t fault = {
			.detail = OVE_LXP_MPU_PROFILE_FAULT,
			.address = 0u,
		};
		(void)lxp_slot_report_memory_fault(task_slot_ref(sidx), &fault);
		regs[REG_PC] = (uint32_t)(uintptr_t)&nuttx_park_entry & ~1u;
		regs[REG_XPSR] |= (1u << 24);
		return 0;
	}

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
	 * preserve it across a parked/resumed syscall. CONFIG_ARCH_FPU makes NuttX save the whole
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

	(void)lxp_dispatch_slot(task_slot_ref(sidx), &f);

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
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	/* Include nested IRQ time: an RT release can preempt this handler, but
	 * Cortex-M cannot dispatch its woken thread until the outer SVC returns.
	 * Reading the endpoint before recording excludes telemetry bookkeeping. */
	uint32_t svc_cycles = (uint32_t)up_perf_gettime() - svc_start_cycles;
	ove_lxp_svc_metrics_record(svc_syscall, svc_cycles);
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

/* map_device (P3): prepare one of this slot's two UNPRIVILEGED device MPU
 * ranges. Programming the hardware here would make the mapping global until
 * another caller changed it; instead the context-switch hook installs the
 * incoming slot's descriptors in regions 5 and 6. size 0 clears every mapping
 * owned by the slot (fresh exec, abort, and run teardown).
 *
 * ARMv7-M MPU regions are power-of-two sized and naturally aligned. Expand the
 * encoded region until the rounded-down base covers the complete requested
 * interval; never silently map a truncated tail. */
static int nuttx_map_device(int sidx, uintptr_t addr, size_t size, unsigned attrs)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || attrs > LXP_MAP_DEV)
		return -1;

	struct nuttx_device_map *device_maps = g_slots[sidx].device_maps;
	irqstate_t flags = enter_critical_section();
	if (size == 0) {
		memset(device_maps, 0, sizeof(g_slots[sidx].device_maps));
		leave_critical_section(flags);
		return 0;
	}

	if (addr > UINTPTR_MAX - size) {
		leave_critical_section(flags);
		return -1;
	}
	uintptr_t end = addr + size;
	size_t rsz = 32u;
	while (rsz < size || (addr & ~(uintptr_t)(rsz - 1u)) > UINTPTR_MAX - rsz ||
	       (addr & ~(uintptr_t)(rsz - 1u)) + rsz < end) {
		if (rsz > SIZE_MAX / 2u) {
			leave_critical_section(flags);
			return -1;
		}
		rsz <<= 1;
	}

	int free_map = -1;
	int map = -1;
	for (unsigned i = 0; i < LXP_DEVICE_MPU_COUNT; i++) {
		if (device_maps[i].used && device_maps[i].addr == addr) {
			map = (int)i;
			break;
		}
		if (!device_maps[i].used && free_map < 0)
			free_map = (int)i;
	}
	if (map < 0)
		map = free_map;
	if (map < 0) {
		leave_critical_section(flags);
		return -1;
	}

	/* TEX/S/C/B from the LXP_MAP_* hint: NC=0 -> Normal non-cacheable,
	 * WT=1 -> Normal write-through, DEV=2 -> Device. */
	uint32_t texscb = (attrs == LXP_MAP_WT) ? 0x02u : (attrs == LXP_MAP_DEV) ? 0x01u : 0x08u;
	device_maps[map] = (struct nuttx_device_map){
		.addr = addr,
		.size = size,
		.rbar = (uint32_t)(addr & ~(uintptr_t)(rsz - 1u)),
		.rasr = (1u << 0) | OVE_MPU_RASR_SIZE(rsz) | (texscb << 16) | (0x3u << 24) |
			(1u << 28),
		.attrs = (uint8_t)attrs,
		.used = 1,
	};
	/* Keep the currently prepared profile valid until the core commits the
	 * shared mm's device_generation. A CLONE_VM sibling cannot then observe
	 * backend-local descriptors paired with the old logical policy between
	 * this staging call and that commit. The changed generation makes the
	 * next switch compile and install the staged descriptors. */
	leave_critical_section(flags);
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

static void *nuttx_park_prepare(int sidx, uint32_t generation, const struct lxp_resume_ctx *ctx)
{
	(void)ctx;
	if (sidx < 0 || sidx >= LXP_NSLOT || g_slots[sidx].pid < 0 ||
	    g_slots[sidx].generation != generation)
		return NULL;
	/* NuttX retains the complete interrupted register set in tcb->xcp.regs.
	 * spawn_resume rewrites that frame before moving the TCB out of the stopped
	 * list, so this port needs no guest-readable handoff descriptor. */
	return NULL;
}

static void slot_task_name(char name[6], int sidx)
{
	name[0] = 'l';
	name[1] = 'n';
	name[2] = 'x';
	if (sidx >= 10) {
		name[3] = (char)('0' + (sidx / 10) % 10);
		name[4] = (char)('0' + sidx % 10);
		name[5] = '\0';
	} else {
		name[3] = (char)('0' + sidx);
		name[4] = '\0';
	}
}

/* Create slot `sidx` with trusted NuttX TLS/stack metadata, then move the
 * architecture context to the guest stack. On Cortex-M exception return the
 * hardware PSP is the end of the exception frame, not REG_SP in the software
 * save area, so relocating this frame is what actually selects guest_sp. */
static int spawn_task(int sidx, uintptr_t guest_sp)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || guest_sp < XCPTCONTEXT_SIZE)
		return -1;

	memset(&g_tcb[sidx], 0, sizeof(g_tcb[sidx]));
	g_tcb[sidx].cmn.flags = TCB_FLAG_TTYPE_TASK; /* static TCB: no FREE_TCB/FREE_STACK */
	char nm[6];
	slot_task_name(nm, sidx); /* diagnostic only; attribution uses the task PID */
	if (nxtask_init(&g_tcb[sidx], nm, SLOT_PRIO, g_nuttx_stacks[sidx], LXP_NUTTX_STACK_SIZE,
			slot_noentry, NULL, NULL, NULL) < 0)
		return -1;

	uint32_t *guest_regs = (uint32_t *)(guest_sp - XCPTCONTEXT_SIZE);
	memcpy(guest_regs, g_tcb[sidx].cmn.xcp.regs, XCPTCONTEXT_SIZE);
	g_tcb[sidx].cmn.xcp.regs = guest_regs;
	g_slots[sidx].pid = g_tcb[sidx].cmn.pid;
	return 0;
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static struct ove_cortex_m_cache_geometry g_lxp_cache_geometry;
static const lxp_cpu_memory_contract_t g_lxp_memory_contract =
	OVE_LXP_MEMORY_CONTRACT_STM32F746_INITIALIZER;
#else
static const lxp_cpu_memory_contract_t g_lxp_memory_contract =
	OVE_LXP_MEMORY_CONTRACT_UNCACHED_INITIALIZER;
#endif

/*
 * Ordinary program/dynamic-pool data needs no launch-time maintenance:
 * coordinator and guest use the same WBWA mapping on the same CPU. Copied
 * text is the one CPU-to-I-cache ownership boundary. The common Cortex-M
 * publisher derives the live line geometry and maintains only those lines,
 * without an RTOS range API that may escalate to a whole-cache operation.
 */
static int nuttx_publish_executable(lxp_region_ref_t address_space, uintptr_t text_lo,
				    size_t text_size)
{
	int ridx = address_space.index;
	if (ridx < 0 || ridx >= LXP_NREG || address_space.generation == 0 || text_size == 0)
		return LXP_ERR_INVALID_PARAM;
	uintptr_t region_lo = (uintptr_t)prog_regions[ridx];
	if (text_lo != region_lo || text_size != LXP_PROG_REGION_SIZE / 2u)
		return LXP_ERR_INVALID_PARAM;

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	if (ove_cortex_m_publish_executable(&g_lxp_cache_geometry, text_lo, text_size) != 0)
		return LXP_ERR_INVALID_PARAM;
#endif
	return LXP_OK;
}

static int nuttx_spawn_launch(int sidx, uint32_t generation, int ridx,
			      const lxp_guest_launch_t *launch)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || generation == 0 || ridx < 0 || ridx >= LXP_NREG ||
	    !launch || g_slots[sidx].pid >= 0)
		return -1;
	lxp_memory_policy_t policy;
	lxp_slot_ref_t slot = {
		.index = (int16_t)sidx,
		.generation = generation,
	};
	if (lxp_slot_memory_policy(slot, &policy) != LXP_OK ||
	    lxp_memory_policy_validate(&policy) != LXP_OK || policy.address_space.index != ridx ||
	    policy.copied_text_base != launch->copied_text_base ||
	    policy.copied_text_size != launch->copied_text_size ||
	    policy.copied_text_executable != (uint8_t)(launch->copied_text_size != 0))
		return -1;
	if (spawn_task(sidx, launch->r[13]) != 0)
		return -1;
	g_slots[sidx].generation = generation;
	uint32_t *regs = g_tcb[sidx].cmn.xcp.regs;
	regs[REG_R0] = launch->r[0];
	regs[REG_R1] = launch->r[1];
	regs[REG_R2] = launch->r[2];
	regs[REG_R3] = launch->r[3];
	regs[REG_R4] = launch->r[4];
	regs[REG_R5] = launch->r[5];
	regs[REG_R6] = launch->r[6];
	regs[REG_R7] = launch->r[7];
	regs[REG_R8] = launch->r[8];
	regs[REG_R9] = launch->r[9];
	regs[REG_R10] = launch->r[10];
	regs[REG_R11] = launch->r[11];
	regs[REG_R12] = launch->r[12];
	regs[REG_SP] = launch->r[13];
	regs[REG_LR] = launch->r[14];
	regs[REG_PC] = launch->r[15] & ~1u;
	regs[REG_XPSR] = launch->xpsr | (1u << 24);
	regs[REG_CONTROL] |=
		CONTROL_NPRIV; /* run UNPRIVILEGED — MPU-restricted to its granted regions */
	nxtask_activate(&g_tcb[sidx].cmn);
	return 0;
}

static int nuttx_spawn_resume(int sidx, uint32_t generation, int ridx, lxp_spawn_resume_mode_t mode,
			      const struct lxp_resume_ctx *ctx, long r0val)
{
	(void)ridx;
	if (sidx < 0 || sidx >= LXP_NSLOT || generation == 0)
		return -1;
	if (mode == LXP_SPAWN_RESUME_PARKED) {
		if (g_slots[sidx].pid < 0 || g_slots[sidx].generation != generation)
			return -1;
		/* Build a native NuttX exception frame immediately below the captured
		 * guest SP. Cortex-M exception return consumes the frame and leaves PSP
		 * exactly at ctx->sp. Reusing the old parked frame is unsafe: it may be
		 * lower on the guest stack after the park entry briefly ran, while merely
		 * changing REG_SP does not move the hardware frame NuttX restores. */
		irqstate_t flags = enter_critical_section();
		if (g_tcb[sidx].cmn.task_state == TSTATE_TASK_STOPPED) {
			uint32_t *regs = (uint32_t *)((uintptr_t)ctx->sp - XCPTCONTEXT_SIZE);
			memset(regs, 0, XCPTCONTEXT_SIZE);
			g_tcb[sidx].cmn.xcp.regs = regs;
			regs[REG_SP] = ctx->sp;
			regs[REG_BASEPRI] = 0;
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
			regs[REG_XPSR] = ctx->xpsr | (1u << 24);
			regs[REG_R0] = (uint32_t)r0val;
#if LXP_ENABLE_FPU_CONTEXT
			for (int i = 0; i < 16; i++)
				regs[REG_S0 + i] = ctx->fp.s[i];
			for (int i = 0; i < 16; i++)
				regs[REG_S16 + i] = ctx->fp.s[16 + i];
			regs[REG_FPSCR] = ctx->fp.fpscr;
#endif
			regs[REG_CONTROL] |= CONTROL_NPRIV;
			regs[REG_EXC_RETURN] = LXP_EXC_RETURN_THREAD;
			dq_rem((dq_entry_t *)&g_tcb[sidx].cmn, &g_stoppedtasks);
			(void)nxsched_add_readytorun(&g_tcb[sidx].cmn);
			leave_critical_section(flags);
			return 0;
		}
		leave_critical_section(flags);
		return -1;
	}
	if (mode != LXP_SPAWN_RESUME_START || g_slots[sidx].pid >= 0)
		return -1;
	if (spawn_task(sidx, (uintptr_t)ctx->sp) != 0)
		return -1;
	g_slots[sidx].generation = generation;
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
	/* Restore the captured child's VFP state into its new native task. */
	if (ctx->fp.active) {
		for (int i = 0; i < 16; i++)
			regs[REG_S0 + i] = ctx->fp.s[i];
		for (int i = 0; i < 16; i++)
			regs[REG_S16 + i] = ctx->fp.s[16 + i];
		regs[REG_FPSCR] = ctx->fp.fpscr;
	}
#endif
	regs[REG_CONTROL] |=
		CONTROL_NPRIV; /* unprivileged — MPU-restricted (resumed vfork/clone child) */
	nxtask_activate(&g_tcb[sidx].cmn);
	return 0;
}

static int nuttx_abort_slot(int sidx, uint32_t generation)
{
	if (sidx < 0 || sidx >= LXP_NSLOT)
		return -1;
	if (g_slots[sidx].pid >= 0 && g_slots[sidx].generation != generation)
		return -1;
	if (g_slots[sidx].pid >= 0) {
		/* Slot teardown is synchronous: a parked task has no later
		 * cancellation point at which a deferred delete could complete.
		 * Its cancellation metadata is now trusted, but forced cancellation
		 * still expresses the required host-side transition semantics. */
		if (g_guest_budget_pid == g_slots[sidx].pid)
			g_guest_budget_pid = -1;
		g_tcb[sidx].cmn.flags |= TCB_FLAG_FORCED_CANCEL;
		if (task_delete(g_slots[sidx].pid) < 0)
			return -1;
	}
	(void)nuttx_map_device(sidx, 0, 0, 0);
	g_slots[sidx].pid = -1;
	g_slots[sidx].generation = 0;
	return 0;
}

static int nuttx_park_slot(int sidx, uint32_t generation)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || !lxp_slot_ref_is_runnable(task_slot_ref(sidx)) ||
	    g_slots[sidx].pid < 0 || g_slots[sidx].generation != generation)
		return -1;
	nxsched_suspend(&g_tcb[sidx].cmn);
	return g_tcb[sidx].cmn.task_state == TSTATE_TASK_STOPPED ? 0 : -1;
}

/* Native saved-frame resume replaces the parked frame before this task becomes
 * runnable again, so the token is intentionally unused. */
static void nuttx_park_entry(void *token)
{
	(void)token;
	for (;;)
		__asm__ volatile("nop");
}

/* Guest entropy source (AT_RANDOM and getrandom). GRND_NONBLOCK makes every
 * host call finite; partial reads are completed under a fixed call budget.
 * Any unavailable, unhealthy, or short source fails closed. */
static int nuttx_random_fill(void *buf, size_t len)
{
	if (!buf && len != 0u)
		return LXP_ERR_INVALID_PARAM;
	uint8_t *out = buf;
	size_t done = 0;
	for (unsigned calls = 0; done < len && calls < 16u; calls++) {
		ssize_t got = getrandom(out + done, len - done, GRND_NONBLOCK);
		if (got > 0) {
			done += (size_t)got;
			continue;
		}
		if (got < 0 && errno == EINTR)
			continue;
		int rc = (got < 0 && errno == EAGAIN) ? LXP_ERR_WOULD_BLOCK : LXP_ERR_BUS_ERROR;
		memset(buf, 0, len);
		return rc;
	}
	if (done == len)
		return LXP_OK;
	memset(buf, 0, len);
	return LXP_ERR_BUS_ERROR;
}

/* Coordinator critical section: disable IRQs around the brief proc-table snapshot. */
static lxp_critical_token_t nuttx_crit_enter(void)
{
	return (lxp_critical_token_t)enter_critical_section();
}
static void nuttx_crit_exit(lxp_critical_token_t token)
{
	leave_critical_section((irqstate_t)token);
}

/* Event wakeup: the coordinator blocks here; the dispatch (svc-interrupt context)
 * posts when a program parks. nxsem_post is interrupt-safe. */
static sem_t g_ev;
static bool g_ev_initialized;
static uint8_t g_irq_install_mask;

enum {
	LXP_IRQ_INSTALLED_SVC = 1u << 0,
	LXP_IRQ_INSTALLED_MEM = 1u << 1,
	LXP_IRQ_INSTALLED_BUS = 1u << 2,
	LXP_IRQ_INSTALLED_USAGE = 1u << 3,
};

static int nuttx_attach_lxp_irq(int irq, xcpt_t handler, uint8_t installed_bit)
{
	int rc = irq_attach(irq, handler, NULL);
	if (rc >= 0)
		g_irq_install_mask |= installed_bit;
	return rc;
}

static void nuttx_event_post(void)
{
	nxsem_post(&g_ev);
}
static void nuttx_event_wait(unsigned ms)
{
	(void)nxsem_tickwait(&g_ev, MSEC2TICK(ms));
}

static lxp_thread_state_t guest_thread_state(const struct tcb_s *tcb)
{
	switch (tcb->task_state) {
	case TSTATE_TASK_RUNNING:
		return LXP_THREAD_STATE_RUNNING;
	case TSTATE_TASK_READYTORUN:
		return LXP_THREAD_STATE_READY;
	case TSTATE_TASK_STOPPED:
		return LXP_THREAD_STATE_SUSPENDED;
	case TSTATE_TASK_INACTIVE:
		return LXP_THREAD_STATE_TERMINATED;
	default:
		return LXP_THREAD_STATE_BLOCKED;
	}
}

static int32_t slot_for_pid(uintptr_t identity)
{
	for (int s = 0; s < LXP_NSLOT; s++)
		if (g_slots[s].pid >= 0 && identity == (uintptr_t)(uint32_t)g_slots[s].pid)
			return s;
	return LXP_THREAD_SLOT_NONE;
}

/*
 * Host enumeration may overflow before NuttX reaches its lnx TCBs. Compact any
 * guest entries out of that bounded result and append every live guest from the
 * seam-owned TCB table with the exact per-slot DWT runtime. This both prevents a
 * display-name dependency and guarantees that LXP can charge guest CPU even
 * when some low-activity host threads remain represented by threads-overflow. */
static int lxp_seam_thread_list(struct lxp_thread_info *o, size_t m, size_t *n)
{
	size_t guest_count = 0;
	for (int s = 0; s < LXP_NSLOT; s++)
		if (g_slots[s].pid >= 0)
			guest_count++;
	size_t host_limit = guest_count < m ? m - guest_count : 0;
	size_t local_n = 0;
	size_t *written = n ? n : &local_n;
	int rc = lxp_ove_thread_snapshot_read(&g_thread_snapshot, o, host_limit, written,
					      slot_for_pid);
	size_t raw_count = *written < host_limit ? *written : host_limit;
	size_t count = 0;

	for (size_t i = 0; i < raw_count; i++) {
		if (o[i].lxp_slot != LXP_THREAD_SLOT_NONE)
			continue;
		if (count != i)
			o[count] = o[i];
		o[count].lxp_slot = LXP_THREAD_SLOT_NONE;
		count++;
	}

	for (int s = 0; s < LXP_NSLOT; s++) {
		if (g_slots[s].pid < 0)
			continue;
		if (count >= m) {
			rc = LXP_ERR_QUEUE_FULL;
			break;
		}

		struct lxp_thread_info *info = &o[count++];
		memset(info, 0, sizeof(*info));
#if CONFIG_TASK_NAME_SIZE > 0
		info->name = g_tcb[s].cmn.name;
#else
		info->name = "lnx";
#endif
		info->identity = (uintptr_t)(uint32_t)g_slots[s].pid;
		info->lxp_slot = s;
		info->state = guest_thread_state(&g_tcb[s].cmn);
		info->priority = (int)g_tcb[s].cmn.sched_priority;
		info->stack_size = g_tcb[s].cmn.adj_stack_size;
		info->state_times.running_us = guest_runtime_us(s);
	}
	*written = count;
	return rc;
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

#define LXP_SYSTEM_VERSION \
	"NuttX " CONFIG_VERSION_STRING " ove-" OVE_BUILD_OVERTOS_REV " lxp-" OVE_BUILD_LXP_REV
_Static_assert(sizeof(LXP_SYSTEM_VERSION) <= 65u, "uname version exceeds Linux utsname field");
static const char *lxp_seam_system_version(void)
{
	return LXP_SYSTEM_VERSION;
}

/* Defined at end of file (they reference the MPU / IRQ helpers declared below);
 * the module's lxp_run() invokes them via g_lxp_host_engine.prepare/.teardown. */
static int nuttx_prepare(void);
static void nuttx_teardown(void);
static int nuttx_validate_memory_contract(const lxp_cpu_memory_contract_t *declared);

const lxp_os_ops_t g_lxp_host_engine = {
	.abi_version = LXP_OS_OPS_ABI_VERSION,
	.struct_size = sizeof(lxp_os_ops_t),
	.prepare = nuttx_prepare,
	.teardown = nuttx_teardown,
	.region = nuttx_region,
	.dyn_pool = nuttx_dyn_pool,
	.exec_capture = nuttx_exec_capture,
	.map_device = nuttx_map_device,
	.spawn_launch = nuttx_spawn_launch,
	.spawn_resume = nuttx_spawn_resume,
	.abort_slot = nuttx_abort_slot,
	.park_entry = nuttx_park_entry,
	.park_prepare = nuttx_park_prepare,
	.park_slot = nuttx_park_slot,
	.crit_enter = nuttx_crit_enter,
	.crit_exit = nuttx_crit_exit,
	.event_post = nuttx_event_post,
	.event_wait = nuttx_event_wait,
	/* Ordinary CPU accesses are coherent because region 1 and the per-guest
	 * overlays use matching attributes. Device/DMA transfers remain explicit. */
	.time_us = ove_time_get_us,
	.time_ns = ove_time_get_ns,
	.thread_list = lxp_seam_thread_list,
	.mem_stats = lxp_seam_mem_stats,
	.system_version = lxp_seam_system_version,
	.publish_executable = nuttx_publish_executable,
	.cpu_memory_contract = &g_lxp_memory_contract,
	.validate_memory_contract = nuttx_validate_memory_contract,
	.random_fill =
		nuttx_random_fill, /* REQUIRED: without it exec() can't seed AT_RANDOM → no launch */
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	.exec_stage = nuttx_exec_stage,
#endif
};

/* ---- unprivileged isolation: MPU region setup ------------------------------ */
/* Run the Linux program UNPRIVILEGED behind a per-program MPU view so a stray/hostile access
 * faults MemManage (contained — see the memfault handler) instead of corrupting the kernel or a
 * sibling program. PRIVDEFENA keeps the privileged kernel on the ARM default map (unchanged); the
 * unprivileged program sees ONLY these regions:
 *   region 0 = code (flash/ROM): unprivileged RO + executable (XN=0) — the shared FDPIC text runs
 *              in-place from the embedded cpio here, and the contained-fault park entry lives here;
 *   region 1 = privileged-only Normal-memory base for coordinator access to the complete pool;
 *   regions 2/3 = the running address space's writable program half and dynamic pool;
 *   region 4 = optional shared QSPI rootfs, user RO + executable;
 *   regions 5/6 = driver-originated device capabilities for the running slot;
 *   region 7 = optional copied-text lower half, user RO + executable.
 * Everything else — kernel .data/.bss/heap, peripherals — is ungranted, so an unprivileged access
 * to it faults. NuttX leaves the MPU disabled in FLAT, so we own it (raw registers; arm_mpu.c is
 * not compiled). */
static void lxp_mpu_init(void)
{
	volatile uint32_t *const mpu_ctrl = (uint32_t *)0xE000ED94u;
	volatile uint32_t *const mpu_rnr = (uint32_t *)0xE000ED98u;
	volatile uint32_t *const mpu_rbar = (uint32_t *)0xE000ED9Cu;
	volatile uint32_t *const mpu_rasr = (uint32_t *)0xE000EDA0u;
	/* prepare() runs privileged with no guest runnable. Rebuild the complete
	 * personality-owned MPU state from a disabled, empty baseline so a
	 * sequential run cannot inherit dynamic regions from its predecessor. */
	*mpu_ctrl = 0u;
	__asm__ volatile("dsb 0xf\nisb 0xf" ::: "memory");
	for (unsigned i = 0; i < 8u; i++) {
		*mpu_rnr = i;
		*mpu_rasr = 0u;
	}
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	const uint32_t code_base = 0x08000000u, code_sz = 19u; /* 1M internal flash */
	const uint32_t code_texscb = 0x02u; /* Normal write-through (real flash) */
	const uint32_t pool_base = 0xC0000000u, pool_sz = 22u; /* 8M external SDRAM (whole pool) */
#else							       /* QEMU mps2-an500 */
	const uint32_t code_base = 0x00000000u,
		       code_sz = 20u;	    /* 2M ROM/flash at 0x0 (kernel .text) */
	const uint32_t code_texscb = 0x08u; /* Normal non-cacheable */
	/* Derived, not spelled again: this region must cover exactly the pool the
	 * prog_regions/dyn_pools pointers above are carved from. Hard-coding it a
	 * second time let the two drift — the base said 8M at 0x60800000 while the
	 * pool had moved to the top 4M, so the region granted the wrong span. */
	const uint32_t pool_base = OVE_LXP_GUEST_POOL_BASE;
	const uint32_t pool_sz = OVE_MPU_RASR_SIZE_FIELD(OVE_LXP_GUEST_POOL_SIZE);
#endif
	/* Region 0: code (shared by every program) — priv RW / unpriv RO (AP=0b010), executable
	 * (XN=0). The FDPIC text runs in-place from the embedded cpio here + the park entry. The
	 * per-program data regions are reprogrammed on every context switch from
	 * the slot's prepared policy, so a running program sees only its own region. */
	*mpu_rnr = 0;
	*mpu_rbar = code_base;
	*mpu_rasr = (1u << 0) | (code_sz << 1) | (code_texscb << 16) | (0x2u << 24);
	/* Region 1: the WHOLE program pool, with the same Normal-memory attributes
	 * as the per-guest overlays, but PRIVILEGED-ONLY (AP=0b001). This base lets
	 * the privileged coordinator/seam touch ANY program's pool region; the
	 * prepared regions 2+3 grant the running program access to only its own region.
	 * On STM32 the 8M region's first 1M subregion is disabled: it contains the
	 * LTDC framebuffer, which must fall through to the default Device mapping
	 * (or a driver-granted device region) rather than becoming cacheable. */
	*mpu_rnr = 1;
	*mpu_rbar = pool_base;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	const uint32_t pool_srd = 1u << 8; /* disable [0xC0000000,0xC0100000) */
#else
	const uint32_t pool_srd = 0u;
#endif
	*mpu_rasr = (1u << 0) | (pool_sz << 1) | pool_srd | (NUTTX_POOL_TEXSCB << 16) |
		    (0x1u << 24) | (1u << 28); /* priv RW, unpriv NO */
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	/* Region 4: the QSPI NOR XIP window. The UNPRIVILEGED guest XIPs its FDPIC text in place
	 * from 0x90000000 → unpriv RO + executable (XN=0), like the internal-flash code region 0.
	 * 16 MB (SIZE=23), 16 MB-aligned = one PMSAv7 region; Normal write-through so the I-cache
	 * absorbs the slow external-flash fetches (RO, so no coherence issue). STATIC and shared by
	 * every program — the switch hook rewrites only regions 2,3,5,6, so region
	 * 4 survives every context switch. */
	*mpu_rnr = 4;
	*mpu_rbar = OVE_LXP_ROOTFS_BASE;
	*mpu_rasr = (1u << 0) | (OVE_MPU_RASR_SIZE_FIELD(OVE_LXP_ROOTFS_SIZE) << 1) |
		    (0x02u << 16) | (0x2u << 24);
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
	*mpu_rbar = OVE_LXP_ROOTFS_BASE;
	*mpu_rasr = (1u << 0) | (OVE_MPU_RASR_SIZE_FIELD(OVE_LXP_ROOTFS_SIZE) << 1) |
		    (0x08u << 16) | (0x2u << 24);
#endif
	/* Device regions are per-slot and installed by lxp_note_resume(). Disable
	 * both before the first guest so a repeated run cannot inherit the previous
	 * run's framebuffer or peripheral view. */
	for (unsigned i = 0; i < LXP_DEVICE_MPU_COUNT; i++) {
		*mpu_rnr = LXP_DEVICE_MPU_FIRST + i;
		*mpu_rasr = 0;
	}
	OVE_SCS_SHCSR |=
		OVE_SHCSR_MEMFAULTENA | OVE_SHCSR_BUSFAULTENA |
		OVE_SHCSR_USGFAULTENA;	   /* MPU faults → MemManage (contained), not HardFault */
	*mpu_ctrl = (1u << 0) | (1u << 2); /* ENABLE | PRIVDEFENA */
	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
}

/* MemManage fault containment. The unprivileged program's stray/hostile access to an ungranted
 * address (kernel RAM, a peripheral, a sibling's region) traps HERE instead of corrupting the
 * system: kill JUST that program (exit 139 = 128+SIGSEGV), park it in the shared entry, and wake
 * the coordinator so its EV_EXIT pass reaps the slot + reports the status to the parent (the shell
 * sees $?=139) — the kernel and any sibling programs run on. A MemManage from a non-program
 * (privileged) context is a genuine kernel bug → chain to NuttX's panicking HardFault path. */
static int lxp_memfault_handler(int irq, void *context, void *arg)
{
	uint32_t *regs = (uint32_t *)context;
	int sidx = (lxp_trap_active() && regs) ? current_slot() : -1;
	if (sidx < 0)
		return arm_hardfault(irq, context, arg); /* privileged kernel fault → NuttX panic */
	uint32_t cfsr = OVE_SCS_CFSR & 0x03ffffffu;
	uintptr_t fault_address = (cfsr & (1u << 7))	? *(volatile uint32_t *)0xE000ED34u
				  : (cfsr & (1u << 15)) ? *(volatile uint32_t *)0xE000ED38u
							: 0u;
	OVE_SCS_CFSR = cfsr; /* write-1-clear the set fault status (MM/Bus/Usage) */
	lxp_guest_fault_t fault = {
		.detail = cfsr,
		.address = fault_address,
	};
	(void)lxp_slot_report_memory_fault(task_slot_ref(sidx), &fault);
	regs[REG_PC] = (uint32_t)(uintptr_t)&nuttx_park_entry & ~1u;
	regs[REG_XPSR] |= (1u << 24); /* keep Thumb state on exception return */
	return 0; /* exception-return: the program spins in the park entry until reaped */
}

/* ---- inter-program isolation: per-program MPU regions on every context switch ---------------- */
/* Program regions 2+3 for the program that owns region `ridx`: region 2 = its data segment
 * (prog_regions[ridx]), region 3 = its dynamic-link arena (dyn_pools[ridx]) — HIGHER priority than
 * the privileged-only whole-pool base (region 1), so for THIS program those two ranges become
 * unprivileged RW while the rest of the pool stays privileged-only (a sibling's region is denied).
 * Both are power-of-2 sized and naturally aligned (the pool base is aligned to the region size and
 * the array stride equals the size), so each maps as one exact MPU region. Execute-never (W^X —
 * code lives in the shared region 0). */
/* Compile one complete logical policy into the four native MPU descriptors
 * used by the switch hook: program, dynamic pool, and two driver capabilities.
 * The registered driver's map_device transition produced the slot's device
 * descriptors; the core snapshot is cross-checked so an arbitrary or stale
 * physical range can never be promoted merely by changing backend-local
 * state. */
static int nuttx_prepare_profile(int sidx, const lxp_memory_policy_t *policy)
{
	if (lxp_memory_policy_validate(policy) != LXP_OK || sidx < 0 || sidx >= LXP_NSLOT ||
	    policy->address_space.index < 0 || policy->address_space.index >= LXP_NREG ||
	    policy->device_count > LXP_DEVICE_MPU_COUNT)
		return -1;
	struct nuttx_prepared_profile *prepared = &g_slots[sidx].profile;
	if (prepared->valid && lxp_memory_policy_matches_key(policy, &prepared->key))
		return 0;

	int ridx = policy->address_space.index;
	memset(prepared, 0, sizeof(*prepared));
	uintptr_t program_base = (uintptr_t)prog_regions[ridx];
	size_t writable_size = LXP_PROG_REGION_SIZE;
	if (policy->copied_text_executable) {
		if (policy->copied_text_base != program_base ||
		    policy->copied_text_size != LXP_PROG_REGION_SIZE / 2u)
			return -1;
		program_base += policy->copied_text_size;
		writable_size -= policy->copied_text_size;
	}
	prepared->rbar[0] = (uint32_t)program_base;
	prepared->rasr[0] = (1u << 0) | OVE_MPU_RASR_SIZE(writable_size) |
			    (NUTTX_POOL_TEXSCB << 16) | (0x3u << 24) | (1u << 28);
	prepared->rbar[1] = (uint32_t)(uintptr_t)dyn_pools[ridx];
	prepared->rasr[1] = (1u << 0) | OVE_MPU_RASR_SIZE(LXP_DYN_POOL_SIZE) |
			    (NUTTX_POOL_TEXSCB << 16) | (0x3u << 24) | (1u << 28);

	unsigned caps = 0;
	for (unsigned i = 0; i < LXP_DEVICE_MPU_COUNT; i++) {
		const struct nuttx_device_map *map = &g_slots[sidx].device_maps[i];
		if (!map->used)
			continue;
		if (caps >= policy->device_count || policy->devices[caps].base != map->addr ||
		    policy->devices[caps].size != map->size ||
		    policy->devices[caps].attrs != map->attrs)
			return -1;
		prepared->rbar[2u + i] = map->rbar;
		prepared->rasr[2u + i] = map->rasr;
		caps++;
	}
	if (caps != policy->device_count)
		return -1;
	if (policy->copied_text_executable) {
		prepared->rbar[4] = (uint32_t)policy->copied_text_base;
		/* The global profile remains installed while privileged coordinator
		 * code runs. AP=2 therefore keeps privileged write access for a later
		 * reload while granting the guest read-only execution. */
		prepared->rasr[4] = (1u << 0) | OVE_MPU_RASR_SIZE(policy->copied_text_size) |
				    (NUTTX_POOL_TEXSCB << 16) | (0x2u << 24);
	}

	const struct ove_cortex_m_mpu_expectation program = {
		.base = program_base,
		.size = writable_size,
		.texscb = NUTTX_POOL_TEXSCB,
		.access = 3u,
		.execute_never = 1u,
	};
	const struct ove_cortex_m_mpu_expectation dynamic = {
		.base = (uintptr_t)dyn_pools[ridx],
		.size = LXP_DYN_POOL_SIZE,
		.texscb = NUTTX_POOL_TEXSCB,
		.access = 3u,
		.execute_never = 1u,
	};
	if (!ove_cortex_m_mpu_descriptor_matches(prepared->rbar[0], prepared->rasr[0], &program) ||
	    !ove_cortex_m_mpu_descriptor_matches(prepared->rbar[1], prepared->rasr[1], &dynamic))
		return -1;
	for (unsigned i = 0; i < LXP_DEVICE_MPU_COUNT; i++) {
		const struct nuttx_device_map *map = &g_slots[sidx].device_maps[i];
		if (!map->used) {
			if (prepared->rasr[2u + i] != 0u)
				return -1;
			continue;
		}
		struct ove_cortex_m_mpu_region native;
		uint8_t texscb = map->attrs == LXP_MAP_WT    ? 0x02u
				 : map->attrs == LXP_MAP_DEV ? 0x01u
							     : 0x08u;
		if (ove_cortex_m_mpu_region_decode(prepared->rbar[2u + i], prepared->rasr[2u + i],
						   &native) != 0 ||
		    native.texscb != texscb || native.access != 3u || native.execute_never != 1u ||
		    !ove_cortex_m_mpu_region_contains(&native, map->addr, map->size))
			return -1;
	}
	if (policy->copied_text_executable) {
		const struct ove_cortex_m_mpu_expectation executable = {
			.base = policy->copied_text_base,
			.size = policy->copied_text_size,
			.texscb = NUTTX_POOL_TEXSCB,
			.access = 2u,
			.execute_never = 0u,
		};
		if (!ove_cortex_m_mpu_descriptor_matches(prepared->rbar[4], prepared->rasr[4],
							 &executable))
			return -1;
	} else if (prepared->rasr[4] != 0u) {
		return -1;
	}
	prepared->key = lxp_memory_policy_make_key(policy);
	prepared->valid = 1u;
	return 0;
}

static int nuttx_profile_live_matches(const struct nuttx_prepared_profile *prepared)
{
	static const uint8_t region[LXP_NATIVE_POLICY_REGIONS] = {2u, 3u, 5u, 6u, 7u};
	struct ove_cortex_m_mpu_snapshot snapshot;
	if (!prepared || !prepared->valid || ove_cortex_m_mpu_snapshot_read(&snapshot) != 0 ||
	    !(snapshot.ctrl & OVE_CORTEX_M_MPU_CTRL_ENABLE))
		return 0;

	for (unsigned i = 0; i < LXP_NATIVE_POLICY_REGIONS; i++) {
		if (region[i] >= snapshot.count)
			return 0;
		if (prepared->rasr[i] == 0u) {
			if (snapshot.regions[region[i]].enabled)
				return 0;
			continue;
		}
		struct ove_cortex_m_mpu_region native;
		if (ove_cortex_m_mpu_region_decode(prepared->rbar[i], prepared->rasr[i], &native) !=
		    0)
			return 0;
		const struct ove_cortex_m_mpu_expectation expected = {
			.base = native.base,
			.size = native.size,
			.subregion_disable = native.subregion_disable,
			.texscb = native.texscb,
			.access = native.access,
			.execute_never = native.execute_never,
		};
		if (!ove_cortex_m_mpu_region_matches_expectation(&snapshot.regions[region[i]],
								 &expected))
			return 0;
		/* Program, arena, and copied-text mappings must also win over every other
		 * descriptor, not merely exist at their expected region numbers. */
		if ((i == 0u || i == 1u || i == 4u) &&
		    !ove_cortex_m_mpu_snapshot_effective_matches(&snapshot, &expected))
			return 0;
	}
	return 1;
}

static int nuttx_install_profile(const struct nuttx_prepared_profile *prepared)
{
	volatile uint32_t *const mpu_rnr = (uint32_t *)0xE000ED98u;
	volatile uint32_t *const mpu_rbar = (uint32_t *)0xE000ED9Cu;
	volatile uint32_t *const mpu_rasr = (uint32_t *)0xE000EDA0u;
	static const uint8_t region[LXP_NATIVE_POLICY_REGIONS] = {2u, 3u, 5u, 6u, 7u};

	if (!prepared || !prepared->valid)
		return -1;
	for (unsigned i = 0; i < LXP_NATIVE_POLICY_REGIONS; i++) {
		*mpu_rnr = region[i];
		if (prepared->rasr[i]) {
			*mpu_rbar = prepared->rbar[i];
			*mpu_rasr = prepared->rasr[i];
		} else {
			*mpu_rasr = 0;
		}
	}
	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
	if (!nuttx_profile_live_matches(prepared))
		return -1;
	g_installed_policy = prepared->key;
	g_installed_policy_valid = 1u;
	return 0;
}

/* A policy snapshot/compile failure must not leave the previous guest's
 * unprivileged regions active. Disable every dynamic region before returning
 * to the incoming task so the fault is contained instead of inheriting stale
 * program or device access. */
static void nuttx_disable_dynamic_regions(void)
{
	volatile uint32_t *const mpu_rnr = (uint32_t *)0xE000ED98u;
	volatile uint32_t *const mpu_rasr = (uint32_t *)0xE000EDA0u;
	static const uint8_t region[LXP_NATIVE_POLICY_REGIONS] = {2u, 3u, 5u, 6u, 7u};

	for (unsigned i = 0; i < LXP_NATIVE_POLICY_REGIONS; i++) {
		*mpu_rnr = region[i];
		*mpu_rasr = 0;
	}
	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
	g_installed_policy_valid = 0u;
}

static int nuttx_profile_is_current(int sidx)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || !g_slots[sidx].profile.valid ||
	    !g_installed_policy_valid)
		return 0;
	const lxp_memory_policy_key_t *prepared = &g_slots[sidx].profile.key;
	lxp_slot_ref_t slot = task_slot_ref(sidx);
	return lxp_slot_ref_equal(prepared->slot, slot) &&
	       lxp_slot_ref_equal(g_installed_policy.slot, slot) &&
	       lxp_region_ref_equal(prepared->address_space, g_installed_policy.address_space) &&
	       prepared->device_generation == g_installed_policy.device_generation &&
	       prepared->exec_generation == g_installed_policy.exec_generation &&
	       prepared->copied_text_base == g_installed_policy.copied_text_base &&
	       prepared->copied_text_size == g_installed_policy.copied_text_size &&
	       prepared->copied_text_executable == g_installed_policy.copied_text_executable;
}

/* Note-driver resume hook — fires on EVERY switch TO a task (sched_note_resume, in
 * sched_switchcontext), INCLUDING round-robin preemption between two runnable program tasks that
 * never enters the seam's svc handler. If the incoming task is a program slot, swap the MPU to its
 * program, dynamic-pool, and device regions so it cannot reach a sibling's. Kernel/coordinator
 * tasks are privileged (PRIVDEFENA), so their region set is irrelevant → skip (leaving the last
 * program's regions is harmless; privileged access uses the default map). Runs in the switch
 * context: bounded register writes + barriers, no allocation, no blocking. */
static void lxp_note_start(struct note_driver_s *drv, struct tcb_s *tcb)
{
	(void)drv;
	if (lxp_trap_active() && tcb)
		ove_nuttx_runtime_start(tcb->pid);
}

static void lxp_note_stop(struct note_driver_s *drv, struct tcb_s *tcb)
{
	(void)drv;
	if (lxp_trap_active() && tcb)
		ove_nuttx_runtime_stop(tcb->pid);
}

static void lxp_note_resume(struct note_driver_s *drv, struct tcb_s *tcb)
{
	(void)drv;
	if (!lxp_trap_active() || !tcb)
		return;
	ove_nuttx_runtime_switch(tcb->pid);
	/* Defensive: this hook fires on EVERY switch, including kernel/coordinator tasks. A valid
	 * tcb lives in on-chip SRAM/DTCM (0x2000_0000..0x2008_0000); anything else would BusFault on
	 * the tcb->pid deref (and no program slot could match a non-RAM pid holder anyway). */
	if ((uintptr_t)tcb < 0x20000000u || (uintptr_t)tcb >= 0x20080000u)
		return;
	pid_t pid = tcb->pid;
	for (int i = 0; i < LXP_NSLOT; i++) {
		if (g_slots[i].pid == pid && lxp_slot_ref_is_runnable(task_slot_ref(i))) {
			/* Preserve a partially consumed slice across host/RT preemption, but
			 * assign a fresh weighted slice when guest ownership changes. All
			 * guests retain SLOT_PRIO; niceness cannot cross an RTOS class. */
			if (g_guest_budget_pid != pid) {
				uint32_t base_ticks = MSEC2TICK(CONFIG_RR_INTERVAL);
				uint32_t weight = lxp_guest_sched_weight(i);
				uint32_t ticks = (base_ticks * weight + 19u) / 20u;
				g_tcb[i].cmn.timeslice = ticks != 0u ? (int32_t)ticks : 1;
				g_guest_budget_pid = pid;
			}
			lxp_memory_policy_t policy;
			if (lxp_slot_memory_policy(task_slot_ref(i), &policy) != LXP_OK ||
			    nuttx_prepare_profile(i, &policy) != 0) {
				nuttx_disable_dynamic_regions();
				return;
			}
			if (!g_installed_policy_valid ||
			    !lxp_memory_policy_matches_key(&policy, &g_installed_policy)) {
				if (nuttx_install_profile(&g_slots[i].profile) != 0)
					nuttx_disable_dynamic_regions();
			}
			return;
		}
	}
}

static const struct note_driver_ops_s g_lxp_note_ops = {
	.start = lxp_note_start,
	.stop = lxp_note_stop,
	.resume = lxp_note_resume,
};
static struct note_driver_s g_lxp_note_driver = {
	.ops = &g_lxp_note_ops,
};
static bool g_lxp_note_registered;

/* Per-run bring-up / teardown (was the body of the old lxp_run() wrapper). The
 * public lxp_run() now lives in the module (src/lxp_run.c) and calls these via
 * g_lxp_host_engine.prepare()/.teardown() around the internal run loop. */
static int nuttx_prepare(void)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	if (ove_cortex_m_cache_geometry_read(&g_lxp_cache_geometry) != 0)
		return -1;
#endif
	g_irq_install_mask = 0;
	g_guest_budget_pid = -1;
	memset(g_slots, 0, sizeof(g_slots));
	g_installed_policy_valid = 0;
	lxp_mpu_init(); /* unprivileged-isolation regions + enable the MPU (both boards) */
	for (int i = 0; i < LXP_NSLOT; i++)
		g_slots[i].pid = -1;
	if (nxsem_init(&g_ev, 0, 0) < 0)
		return -1;
	g_ev_initialized = true;
	if (nuttx_attach_lxp_irq(LXP_IRQ_SVCALL, lxp_svc_handler, LXP_IRQ_INSTALLED_SVC) < 0 ||
	    nuttx_attach_lxp_irq(LXP_IRQ_MEMFAULT, lxp_memfault_handler, LXP_IRQ_INSTALLED_MEM) <
		    0 ||
	    nuttx_attach_lxp_irq(LXP_IRQ_BUSFAULT, lxp_memfault_handler, LXP_IRQ_INSTALLED_BUS) <
		    0 ||
	    nuttx_attach_lxp_irq(LXP_IRQ_USGFAULT, lxp_memfault_handler, LXP_IRQ_INSTALLED_USAGE) <
		    0)
		return -1;
	/* NuttX has no public note-driver unregister API. Register this static
	 * driver once, then reset its per-run accounting state on every launch. */
	if (!g_lxp_note_registered) {
		if (note_driver_register(&g_lxp_note_driver) < 0)
			return -1;
		g_lxp_note_registered = true;
	}
	ove_nuttx_runtime_reset(getpid());
	return 0;
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static int nuttx_validate_static_mpu(void)
{
	struct ove_cortex_m_mpu_snapshot snapshot;
	if (ove_cortex_m_mpu_snapshot_read(&snapshot) != 0 || snapshot.count != 8u ||
	    (snapshot.ctrl & (OVE_CORTEX_M_MPU_CTRL_ENABLE | OVE_CORTEX_M_MPU_CTRL_PRIVDEFENA)) !=
		    (OVE_CORTEX_M_MPU_CTRL_ENABLE | OVE_CORTEX_M_MPU_CTRL_PRIVDEFENA))
		return 0;

	const struct ove_cortex_m_mpu_region *pool = &snapshot.regions[1];
	return ove_cortex_m_mpu_region_matches(pool, 0xc0000000u, 8u * 1024u * 1024u, 1u, 0x0bu, 1u,
					       1u) &&
	       ove_cortex_m_mpu_region_contains(pool, OVE_LXP_GUEST_POOL_BASE,
						OVE_LXP_GUEST_POOL_SIZE) &&
	       !ove_cortex_m_mpu_region_overlaps_enabled(pool, 0xc0000000u, 1024u * 1024u);
}
#endif

static int nuttx_validate_memory_contract(const lxp_cpu_memory_contract_t *declared)
{
	if (declared != &g_lxp_memory_contract)
		return LXP_ERR_INVALID_PARAM;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	return ove_lxp_memory_contract_matches_cache(declared, &g_lxp_cache_geometry) &&
			       nuttx_validate_static_mpu()
		       ? LXP_OK
		       : LXP_ERR_INVALID_PARAM;
#else
	return (OVE_SCB_CCR & OVE_SCB_CCR_DC) == 0u ? LXP_OK : LXP_ERR_INVALID_PARAM;
#endif
}

static void nuttx_teardown(void)
{
	nuttx_disable_dynamic_regions();
	for (int i = 0; i < LXP_NSLOT; i++) {
		memset(g_slots[i].device_maps, 0, sizeof(g_slots[i].device_maps));
		memset(&g_slots[i].profile, 0, sizeof(g_slots[i].profile));
	}
	g_installed_policy_valid = 0;
	if (g_irq_install_mask & LXP_IRQ_INSTALLED_SVC)
		irq_attach(LXP_IRQ_SVCALL, arm_svcall, NULL);
	if (g_irq_install_mask & LXP_IRQ_INSTALLED_MEM)
		irq_attach(LXP_IRQ_MEMFAULT, arm_hardfault, NULL);
	if (g_irq_install_mask & LXP_IRQ_INSTALLED_BUS)
		irq_attach(LXP_IRQ_BUSFAULT, arm_hardfault, NULL);
	if (g_irq_install_mask & LXP_IRQ_INSTALLED_USAGE)
		irq_attach(LXP_IRQ_USGFAULT, arm_hardfault, NULL);
	g_irq_install_mask = 0;
	if (g_ev_initialized) {
		nxsem_destroy(&g_ev);
		g_ev_initialized = false;
	}
}

#endif /* CONFIG_OVE_LINUX */
