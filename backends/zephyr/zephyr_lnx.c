/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr seam for the Linux personality. The engine-agnostic run loop, svc
 * dispatch, and signal delivery live in modules/lxp/src/lxp_run.c; this file
 * supplies only the Zephyr-specific bits: the svc trap, the program memory + MPU
 * domains, and the task spawn (via the lxp_engine vtable).
 *
 * Each program runs as an UNPRIVILEGED K_USER thread in its own k_mem_domain, while
 * the privileged coordinator stays in the default domain and can reload any slot.
 * The domain is board-specific: AN521/PMSAv8 uses libc + Zephyr malloc + rootfs +
 * image + arena partitions; STM32F746/PMSAv7 XIPs the rootfs through a static
 * QSPI mapping, drops the unused Zephyr-malloc partition, and uses libc + image +
 * arena. Both also consume the K_USER stack region. The resume context stays in
 * the guest's own image region so it costs no additional MPU partition. The
 * program's svc #0 is an unprivileged fault routed by Zephyr to
 * z_do_kernel_oops, which we --wrap.
 */

#include <zephyr/kernel.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/arch/exception.h>
#include <zephyr/init.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/random/random.h> /* sys_csrand_get -> engine random_fill op */
#include <zephyr/sys/printk.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/version.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "lxp/lxp_exec.h"
#include "lxp/lxp_seam.h"
#include "ove/build.h"
#include "ove/lxp_memory_layout.h"
#include "ove/time.h"	/* ove_time_get_us/ns -> engine time_us/time_ns ops (pulls ove_config.h) */
#include "ove/thread.h" /* ove_thread_list -> engine thread_list op */
#include "ove/hal/hal_fb.h"
#include "lxp_ove_thread_adapter.h"
#include "ove_cortex_m_cache.h"
#include "ove_cortex_m_mpu.h"
#include "ove_lxp_memory_contract.h"
#include "ove_zephyr_priority.h"
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
#include "ove/lxp_metrics.h"
#include "ove_zephyr_lnx_metrics.h"
#endif

BUILD_ASSERT(IS_ENABLED(CONFIG_USERSPACE),
	     "the Zephyr Linux personality requires unprivileged user threads");
BUILD_ASSERT(CONFIG_MAIN_THREAD_PRIORITY == OVE_ZEPHYR_PRIO_LXP_COORDINATOR,
	     "LXP coordinator priority must match the Zephyr priority contract");
BUILD_ASSERT(CONFIG_SYSTEM_WORKQUEUE_PRIORITY == OVE_ZEPHYR_PRIO_SYSTEM_WORKQUEUE,
	     "system workqueue priority must match the Zephyr priority contract");
BUILD_ASSERT(OVE_ZEPHYR_PRIO_CRITICAL < OVE_ZEPHYR_PRIO_LXP_COORDINATOR &&
		     OVE_ZEPHYR_PRIO_LXP_COORDINATOR < OVE_ZEPHYR_PRIO_LXP_GUEST,
	     "critical, coordinator, and guest priorities must remain ordered");
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
BUILD_ASSERT(IS_ENABLED(CONFIG_CSPRNG_ENABLED) && IS_ENABLED(CONFIG_HARDWARE_DEVICE_CS_GENERATOR),
	     "STM32 Linux guests require a hardware-backed CSPRNG");
#endif
BUILD_ASSERT(IS_ENABLED(CONFIG_EXCEPTION_DUMP_HOOK_ONLY),
	     "the Linux personality needs selective exception-dump routing");
#if defined(CONFIG_NETWORKING)
BUILD_ASSERT(IS_ENABLED(CONFIG_NET_TC_THREAD_PREEMPTIVE),
	     "Linux network traffic classes must be preemptible");
BUILD_ASSERT(CONFIG_NET_TC_RX_THREAD_BASE_PRIO == OVE_ZEPHYR_PRIO_NET_TC &&
		     CONFIG_NET_TC_TX_THREAD_BASE_PRIO == OVE_ZEPHYR_PRIO_NET_TC,
	     "network traffic-class priorities must match the Zephyr priority contract");
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
BUILD_ASSERT(IS_ENABLED(CONFIG_ETH_STM32_HAL_RX_THREAD_PREEMPTIVE),
	     "STM32 Ethernet RX must be preemptible");
BUILD_ASSERT(CONFIG_ETH_STM32_HAL_RX_THREAD_PRIO == OVE_ZEPHYR_PRIO_ABOVE_NORMAL,
	     "STM32 Ethernet RX priority must match the Zephyr priority contract");
#endif
#endif

/* The program-image regions live in a NOLOAD external-RAM linker region: RAM-resident but ZERO
 * flash cost — Zephyr's app_smem is a *loaded* section, so a K_APP_BMEM array this big would store
 * the zero-init regions as that many MB of flash. External RAM is also a region separate from the
 * kernel SRAM, so the per-program MPU partitions built in setup_domain() don't overlap the kernel's
 * region (the reason app_smem was used). The node differs per board: an521 uses the final 1088 KiB
 * of PSRAM (ove-psram.overlay → "OVE_PROG_RAM"); the real STM32F746 uses the 8 MB FMC SDRAM @0xC0000000
 * (upstream `sdram1` node → "SDRAM1"; the LTDC display that would share it is disabled in the
 * linux_interop overlay). */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#define OVE_PROG_RAM_NODE DT_NODELABEL(sdram1)
#else
#define OVE_PROG_RAM_NODE DT_NODELABEL(lxp_pool_ram)
#endif
/* Per-program partition base alignment. On a power-of-2 MPU (PMSAv7, e.g. the STM32F746, where
 * CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT=y) an MPU region's base must be aligned to its SIZE, so
 * each per-program partition (one row of these arrays) must start on a PROG_REGION/DYN_POOL boundary
 * — otherwise the hardware aligns the base DOWN at context switch and the region spans the wrong
 * range, so the unprivileged program's data/IO lands in unmapped memory (it launches but silently
 * relays nothing). PMSAv8 (the an521) allows a 32-byte-aligned base, so it needs no size alignment
 * (and avoids the padding). */
#if defined(CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT)
#define LXP_EXT_STORAGE_ALIGN LXP_DYN_POOL_SIZE
#else
#define LXP_EXT_STORAGE_ALIGN 32
#endif
/* Largest-alignment rows first, then program rows and cold coordinator state.
 * Keeping them in one object makes odd region counts safe and prevents linker
 * padding between independently aligned arrays. */
struct lxp_ext_storage {
	uint8_t dyn_pools[LXP_NREG][LXP_DYN_POOL_SIZE];
	uint8_t prog_regions[LXP_NREG][LXP_PROG_REGION_SIZE];
	lxp_exec_capture_t exec_captures[LXP_NSLOT];
	struct lxp_ove_thread_snapshot thread_snapshot;
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	uint8_t netfs_exec_stage[256u * 1024u];
#endif
};
static struct lxp_ext_storage
	g_lxp_ext_storage Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(OVE_PROG_RAM_NODE))
		__aligned(LXP_EXT_STORAGE_ALIGN);
_Static_assert(offsetof(struct lxp_ext_storage, prog_regions) % LXP_PROG_REGION_SIZE == 0,
	       "program rows must be aligned to their MPU region size");
#define dyn_pools (g_lxp_ext_storage.dyn_pools)
#define prog_regions (g_lxp_ext_storage.prog_regions)
#define g_exec_captures (g_lxp_ext_storage.exec_captures)
#define g_thread_snapshot (g_lxp_ext_storage.thread_snapshot)
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
#define g_netfs_exec_stage (g_lxp_ext_storage.netfs_exec_stage)
#endif

#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
/* Staging buffer for a fetched remote ELF image (netfs exec-off-mount): the 9P client fills it from
 * the mount, then the loader COPIES its text into the program region. Lives in the same external-RAM
 * NOLOAD region as the program pools (SDRAM1 on the real F746 / PSRAM on the an521) → RAM-resident,
 * zero flash cost. Sized for a small/medium FDPIC binary: only the exec's OWN text+data is fetched;
 * its libc.so/ld.so stay XIP from the LOCAL rootfs cpio. Mirrors the FreeRTOS/NuttX g_netfs_exec_stage. */
static uint8_t *zephyr_exec_stage(size_t *cap)
{
	if (cap)
		*cap = sizeof(g_netfs_exec_stage);
	return g_netfs_exec_stage;
}
#endif

/* Per-process MPU domain. A privileged native entry performs Zephyr's thread
 * startup, then drops directly into the guest. Guest execution consequently
 * needs only its program, dynamic arena, and optional rootfs partitions; no
 * Zephyr libc or malloc partition is exposed. */
#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
/* QEMU injects the CPIO into PSRAM instead of embedding it in Zephyr's static
 * user-RX text. Give every guest the reserved, read/execute-only rootfs window. */
static struct k_mem_partition g_rootfs_partition = {
	.start = OVE_LXP_ROOTFS_BASE,
	.size = OVE_LXP_ROOTFS_SIZE,
	.attr = K_MEM_PARTITION_P_RX_U_RX,
};
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(lxp_rootfs_ram)) == OVE_LXP_ROOTFS_BASE,
	     "generated rootfs base must match devicetree");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(lxp_rootfs_ram)) == OVE_LXP_ROOTFS_SIZE,
	     "generated rootfs size must match devicetree");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(lxp_pool_ram)) == OVE_LXP_GUEST_POOL_BASE,
	     "generated guest-pool base must match devicetree");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(lxp_pool_ram)) == OVE_LXP_GUEST_POOL_SIZE,
	     "generated guest-pool size must match devicetree");
BUILD_ASSERT(OVE_LXP_ROOTFS_END == OVE_LXP_GUEST_POOL_BASE,
	     "AN521 rootfs and guest-pool ranges must be adjacent");
#endif
struct zephyr_lxp_region {
	struct k_mem_partition program;
	struct k_mem_partition dynamic;
	struct k_mem_partition executable;
	lxp_memory_policy_key_t policy;
	uint8_t initialized;
	uint8_t policy_valid;
};
/* Keep kernel objects in a native array so Zephyr can describe it compactly. */
static struct k_mem_domain g_domains[LXP_NREG];
static struct zephyr_lxp_region g_regions[LXP_NREG];

/* Guest program pool: Normal write-back write-allocate, NON-shareable, CACHEABLE — deliberately the
 * SAME memory attribute the privileged run loop sees through Zephyr's static SDRAM region. The two
 * compiled MPU regions cover flash and internal SRAM; CONFIG_MEM_ATTR appends the devicetree
 * sdram1 ATTR_MPU_RAM region before dynamic thread partitions are programmed. Coordinator and guest
 * therefore use one coherent cacheable view on this single M7 core, with no handoff maintenance.
 * NON-shareable is essential: the core has no snoop unit and precise-BusFaults on shareable Normal
 * FMC accesses. XN — the guest's text XIPs from the RO cpio, never this data pool. Cacheable also
 * speeds the render (the LVGL draw buffer lives here). an521/QEMU has no cache model, so this is a
 * functional no-op there. */
#define OVE_MEM_PART_RW_CACHE K_MEM_PARTITION_P_RW_U_RW

struct zephyr_lxp_slot {
	k_tid_t tid;
	uint32_t generation;
	lxp_memory_policy_key_t policy;
	uint8_t live_validated;
	uint8_t policy_valid;
};
static struct zephyr_lxp_slot g_slots[LXP_NSLOT];
/* Zephyr owns the opaque thread and stack storage; the seam owns g_slots. */
static struct k_thread g_thread_storage[LXP_NSLOT];
K_THREAD_STACK_ARRAY_DEFINE(g_tramp_stacks, LXP_NSLOT, 1024);
static void zephyr_park_entry(void *token);
static int zephyr_validate_active_profile(int sidx);

static lxp_slot_ref_t task_slot_ref(int slot)
{
	return (lxp_slot_ref_t){
		.index = (int16_t)slot,
		.generation = slot >= 0 && slot < LXP_NSLOT ? g_slots[slot].generation : 0,
	};
}

static int current_slot(void)
{
	k_tid_t t = k_current_get();
	for (int i = 0; i < LXP_NSLOT; i++)
		if (g_slots[i].tid == t && lxp_slot_ref_is_runnable(task_slot_ref(i)))
			return i;
	return -1;
}

/* Zephyr normally prints a long register/fault decode from fault context before
 * k_sys_fatal_error_handler() gets the chance to contain an unprivileged guest.
 * Route exception output through a hook: trusted runtime faults retain Zephyr's
 * complete synchronous dump, while a fault belonging to a Linux guest is
 * represented by the bounded coordinator-context guest-exit record instead.
 * Dropping the va_list avoids both formatting and a millisecond-scale UART
 * critical path. */
static volatile uint32_t g_guest_fault_dump_lines;

static void lxp_exception_dump(const char *format, va_list args)
{
	if (lxp_trap_active() && current_slot() >= 0) {
		g_guest_fault_dump_lines++;
		return;
	}
	vprintk(format, args);
}

static void lxp_exception_drain(bool flush)
{
	ARG_UNUSED(flush);
	/* vprintk is synchronous; discarded guest output has nothing to drain. */
}

static int lxp_exception_dump_init(void)
{
	arch_exception_set_dump_hook(lxp_exception_dump, lxp_exception_drain);
	return 0;
}
/* Install immediately after Zephyr establishes C runtime state so faults from
 * later kernel/driver initialization are forwarded too, not silently dropped
 * by CONFIG_EXCEPTION_DUMP_HOOK_ONLY before main starts. */
SYS_INIT(lxp_exception_dump_init, PRE_KERNEL_1, 0);

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
/* The coordinator is the sole critical-metric writer. Window switching uses
 * the same single-core handoff as the common SVC accumulator. */
static volatile uint32_t g_critical_metrics_active;
static struct ove_zephyr_lnx_critical_metrics g_critical_metrics_window[2];
static volatile uint32_t g_critical_metrics_total_seq;
static struct ove_zephyr_lnx_critical_metrics g_critical_metrics_total;

static void critical_metrics_add(struct ove_zephyr_lnx_critical_metrics *metrics, uint32_t cycles)
{
	if (cycles > metrics->max_cycles)
		metrics->max_cycles = cycles;
	metrics->sections++;
	metrics->total_cycles += cycles;
}

static void critical_metrics_record(uint32_t cycles)
{
	uint32_t active = g_critical_metrics_active;
	critical_metrics_add(&g_critical_metrics_window[active], cycles);

	g_critical_metrics_total_seq++;
	__asm__ volatile("" ::: "memory");
	critical_metrics_add(&g_critical_metrics_total, cycles);
	__asm__ volatile("" ::: "memory");
	g_critical_metrics_total_seq++;
}

void ove_zephyr_lnx_critical_metrics_take(struct ove_zephyr_lnx_critical_metrics *window,
					  struct ove_zephyr_lnx_critical_metrics *total)
{
	uint32_t old_active = g_critical_metrics_active;
	g_critical_metrics_active = old_active ^ 1u;
	__asm__ volatile("" ::: "memory");

	*window = g_critical_metrics_window[old_active];
	g_critical_metrics_window[old_active] = (struct ove_zephyr_lnx_critical_metrics){0};

	uint32_t before;
	uint32_t after;
	do {
		before = g_critical_metrics_total_seq;
		__asm__ volatile("" ::: "memory");
		*total = g_critical_metrics_total;
		__asm__ volatile("" ::: "memory");
		after = g_critical_metrics_total_seq;
	} while (before != after || (after & 1u) != 0u);
}

uint32_t ove_lxp_metrics_counter_hz(void)
{
	return sys_clock_hw_cycles_per_sec();
}
#endif /* CONFIG_OVE_LINUX_RT_SCOPE */

/* ---- the Linux SVC seam ---------------------------------------------------- */
extern void __real_z_do_kernel_oops(const struct arch_esf *esf, _callee_saved_t *callee,
				    uint32_t exc_return);

static __attribute__((noinline, used)) void
zephyr_lnx_kernel_oops_c(const struct arch_esf *esf, _callee_saved_t *callee, uint32_t exc_return)
{
	if (lxp_trap_active()) {
		const uint16_t *svc = (const uint16_t *)(esf->basic.pc - 2);
		if ((*svc & 0xff00u) == 0xdf00u && (*svc & 0x00ffu) == 0x00u) {
			int sidx = current_slot();
			if (sidx >= 0) {
				if (!zephyr_validate_active_profile(sidx)) {
					lxp_guest_fault_t fault = {
						.detail = OVE_LXP_MPU_PROFILE_FAULT,
						.address = 0u,
					};
					(void)lxp_slot_report_memory_fault(task_slot_ref(sidx),
									   &fault);
					((struct arch_esf *)esf)->basic.pc =
						(uint32_t)(uintptr_t)&zephyr_park_entry & ~1u;
					((struct arch_esf *)esf)->basic.xpsr |= (1u << 24);
					return;
				}
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
				uint32_t syscall = callee->v4;
				uint32_t svc_start_cycles = k_cycle_get_32();
#endif
				struct arch_esf *e = (struct arch_esf *)esf;
				struct lxp_frame f;
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
				uint32_t fp_bytes = 0;
#if LXP_ENABLE_FPU_CONTEXT
				/* Hard-float guest: capture its full VFP state. With CONFIG_FPU_SHARING +
				 * lazy stacking, an extended exception frame (EXC_RETURN bit 4 == 0) carries
				 * s0-s15 + FPSCR in esf->fpu; s16-s31 are live in the FPU. Mirror the FreeRTOS
				 * seam: force the pending lazy store, then read the frame + high registers. */
				static struct lxp_fp_context
					fpctx; /* off the deep fault-path stack */
				memset(&fpctx, 0, sizeof(fpctx));
				f.fp = &fpctx;
				if ((exc_return & (1u << 4)) == 0) {
					__asm__ volatile("vpush {s0}\n vpop {s0}\n" ::: "memory");
					for (int i = 0; i < 16; i++)
						fpctx.s[i] = esf->fpu.s[i];
					__asm__ volatile("vstmia %0, {s16-s31}"
							 :
							 : "r"(&fpctx.s[16])
							 : "memory");
					fpctx.fpscr = esf->fpu.fpscr;
					fpctx.active = 1;
					fp_bytes = 18u * sizeof(uint32_t);
				}
#endif
				/* callee->psp points at the HW-stacked frame (8 words, +18 for an extended
				 * FP frame); the pre-svc SP is past it (+4 if the stacker 8-byte-aligned). */
				f.r[13] = callee->psp + 32u + fp_bytes +
					  ((esf->basic.xpsr & (1u << 9)) ? 4u : 0u);
				f.r[14] = esf->basic.lr;
				f.r[15] = esf->basic.pc;
				f.xpsr = esf->basic.xpsr;

				(void)lxp_dispatch_slot(task_slot_ref(sidx), &f);

				e->basic.r0 = f.r[0];
				e->basic.r1 = f.r[1];
				e->basic.r2 = f.r[2];
				e->basic.r3 = f.r[3];
				e->basic.ip = f.r[12];
				e->basic.lr = f.r[14];
				e->basic.pc = f.r[15];
				e->basic.xpsr = f.xpsr;
				/* rt_sigreturn restores r9 from the interrupted FDPIC module
				 * after the handler ran with its own GOT. Write every
				 * callee-saved register back to Zephyr's software copy; the
				 * assembly wrapper below reloads that copy into the live
				 * registers before svc.S performs exception return. */
				callee->v1 = f.r[4];
				callee->v2 = f.r[5];
				callee->v3 = f.r[6];
				callee->v4 = f.r[7];
				callee->v5 = f.r[8];
				callee->v6 = f.r[9];
				callee->v7 = f.r[10];
				callee->v8 = f.r[11];
#if LXP_ENABLE_FPU_CONTEXT
				if ((exc_return & (1u << 4)) == 0) {
					for (int i = 0; i < 16; i++)
						e->fpu.s[i] = fpctx.s[i];
					e->fpu.fpscr = fpctx.fpscr;
					__asm__ volatile("vldmia %0, {s16-s31}"
							 :
							 : "r"(&fpctx.s[16])
							 : "memory");
				}
#endif
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
				ove_lxp_svc_metrics_record(syscall,
							   k_cycle_get_32() - svc_start_cycles);
#endif
				return;
			}
		}
	}
	__real_z_do_kernel_oops(esf, callee, exc_return);
}

/* Zephyr's Cortex-M svc.S builds a temporary _callee_saved_t for
 * z_do_kernel_oops(), then drops it by moving MSP instead of popping r4-r11.
 * That is normally correct because the kernel's oops handler treats the copy as
 * diagnostic-only, but LXP rt_sigreturn must change r9 (the FDPIC GOT). Preserve
 * the pointer across the C helper and explicitly reload the possibly modified
 * copy before returning to svc.S. The two-word push keeps the public C call
 * 8-byte stack aligned. */
_Static_assert(offsetof(_callee_saved_t, v1) == 0u, "callee r4 offset");
_Static_assert(offsetof(_callee_saved_t, v8) == 7u * sizeof(uint32_t), "callee r11 offset");

__attribute__((naked)) void __wrap_z_do_kernel_oops(const struct arch_esf *esf,
						    _callee_saved_t *callee, uint32_t exc_return)
{
	__asm__ volatile("push {r1, lr}\n"
			 "bl zephyr_lnx_kernel_oops_c\n"
			 "pop {r1, lr}\n"
			 "ldmia r1, {r4-r11}\n"
			 "bx lr\n");
}

/* ---- thread entry trampoline ----------------------------------------------- */
#if LXP_ENABLE_FPU_CONTEXT
/* Restore the guest's VFP state (ctx.fp) before the trampoline hands control back — r1 still holds
 * ctx here. Offsets pinned so a resume_ctx layout change is a build error, not silent corruption. */
_Static_assert(offsetof(struct lxp_resume_ctx, fp.s) == 64u, "resume fp.s offset");
_Static_assert(offsetof(struct lxp_resume_ctx, fp.fpscr) == 192u, "resume fp.fpscr offset");
_Static_assert(offsetof(struct lxp_resume_ctx, fp.active) == 196u, "resume fp.active offset");
#define LXP_ZTRAMP_RESTORE_FP                      \
	"ldr r2, [r1, #196]\n" /* ctx.fp.active */ \
	"cmp r2, #0\n"                             \
	"beq 1f\n"                                 \
	"add r2, r1, #64\n" /* &ctx.fp.s[0] */     \
	"vldmia r2, {s0-s31}\n"                    \
	"ldr r2, [r1, #192]\n" /* ctx.fp.fpscr */  \
	"vmsr fpscr, r2\n"                         \
	"1:\n"
#else
#define LXP_ZTRAMP_RESTORE_FP ""
#endif

/* Resume a parked program at a captured context with a chosen r0 (vfork return). */
static void resume_tramp(void *r0val, void *ctx, void *unused)
{
	ARG_UNUSED(unused);
	register void *rv __asm__("r0") = r0val;
	register void *c __asm__("r1") = ctx;
	__asm__ volatile("mov r3, r1\n" LXP_ZTRAMP_RESTORE_FP "ldmia r3!, {r4-r11}\n"
			 "ldr r12, [r3], #4\n"
			 "ldr lr, [r3], #4\n"
			 "ldr r1, [r3], #4\n" /* ctx.sp (temp) */
			 "ldr r2, [r3], #4\n" /* ctx.pc (temp); r3 -> ctx.r1 */
			 "mov sp, r1\n"
			 /* Load flags before push overwrites ctx.xpsr at sp-4. */
			 "ldr r1, [r3, #12]\n"
			 "msr APSR_nzcvq, r1\n"
			 "push {r2}\n"
			 "ldr r1, [r3]\n"
			 "ldr r2, [r3, #4]\n"
			 "ldr r3, [r3, #8]\n"
			 "pop {pc}\n"
			 :
			 : "r"(rv), "r"(c)
			 : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "lr", "cc",
			   "memory");
	__builtin_unreachable();
}

/* Enter through Zephyr's supervisor path so z_thread_entry may touch its
 * private libc/TLS state before the thread becomes unprivileged. The one-way
 * drop then jumps directly to resume_tramp; no Zephyr userspace library state
 * belongs in a Linux guest domain. */
static void zephyr_guest_user_enter(void *r0val, void *ctx, void *unused)
{
	k_thread_user_mode_enter(resume_tramp, r0val, ctx, unused);
	CODE_UNREACHABLE;
}

static void *zephyr_park_prepare(int sidx, uint32_t generation, const struct lxp_resume_ctx *ctx)
{
	ARG_UNUSED(ctx);
	if (sidx < 0 || sidx >= LXP_NSLOT || !g_slots[sidx].tid ||
	    g_slots[sidx].generation != generation)
		return NULL;
	/* PendSV retains the exact svc exception frame and callee-saved
	 * registers in the parked k_thread. spawn_resume rewrites that existing
	 * native frame before returning the thread to the ready queue. */
	return NULL;
}

/* SVC #0 returns a parked guest here without changing PSP. The coordinator is
 * higher priority and suspends it at the pending exception return. Keeping the
 * trampoline naked guarantees the saved native frame remains at the captured
 * PSP; spawn_resume replaces that frame before the thread can execute again. */
static void __attribute__((naked)) zephyr_park_entry(void *token __attribute__((unused)))
{
	__asm__ volatile("1: b 1b\n");
}

/* (Re)build region ridx's MPU domain (W^X text/data split) only when the
 * address-space/device/execute policy changed. Per-slot keys are retained
 * separately because several CLONE_VM threads may bind the same native domain. */
static int setup_domain(int sidx, uint32_t generation, int ridx)
{
	lxp_memory_policy_t policy;
	lxp_slot_ref_t slot = {
		.index = (int16_t)sidx,
		.generation = generation,
	};
	if (lxp_slot_memory_policy(slot, &policy) != LXP_OK ||
	    lxp_memory_policy_validate(&policy) != LXP_OK || policy.address_space.index != ridx ||
	    policy.device_count != 0)
		return -1;
	struct zephyr_lxp_region *state = &g_regions[ridx];
	if (state->policy_valid &&
	    lxp_memory_policy_address_space_matches_key(&policy, &state->policy)) {
		g_slots[sidx].policy = lxp_memory_policy_make_key(&policy);
		g_slots[sidx].policy_valid = 1u;
		g_slots[sidx].live_validated = 0u;
		return 0;
	}
	uint8_t *region = prog_regions[ridx];
	if (state->initialized) {
		k_mem_domain_remove_partition(&g_domains[ridx], &state->program);
		k_mem_domain_remove_partition(&g_domains[ridx], &state->dynamic);
		if (state->executable.size != 0u)
			k_mem_domain_remove_partition(&g_domains[ridx], &state->executable);
	}
	/* The full program region is always RW+XN. A copied executable gets a
	 * higher-priority RO+X prefix overlay; writable load state, descriptors
	 * and the Linux stack remain in the tail. */
	state->program.start = (uintptr_t)region;
	state->program.size = LXP_PROG_REGION_SIZE;
	state->program.attr = OVE_MEM_PART_RW_CACHE;
	state->dynamic.start = (uintptr_t)dyn_pools[ridx];
	state->dynamic.size = LXP_DYN_POOL_SIZE;
	state->dynamic.attr = OVE_MEM_PART_RW_CACHE;
	memset(&state->executable, 0, sizeof(state->executable));
	if (policy.copied_text_executable) {
		if (policy.copied_text_base != (uintptr_t)region || policy.copied_text_size == 0u ||
		    policy.copied_text_size >= LXP_PROG_REGION_SIZE)
			return -1;
		state->executable.start = policy.copied_text_base;
		state->executable.size = policy.copied_text_size;
		state->executable.attr = K_MEM_PARTITION_P_RX_U_RX;
	}
	if (!state->initialized) {
#if defined(CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT)
		/* The guest starts in a privileged shim and drops directly into the
		 * personality trampoline, so no Zephyr libc/TLS code executes in
		 * user mode. All three available partitions therefore belong to the
		 * guest: program RW+XN, dynamic RW+XN, copied text RO+X. */
		if (k_mem_domain_init(&g_domains[ridx], 0, NULL) != 0)
			return -1;
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
		/* AN521's QEMU-loaded rootfs is not a static mapping. Zephyr libc and
		 * malloc are absent for the same direct-entry reason as PMSAv7. */
		struct k_mem_partition *base[] = {&g_rootfs_partition};
		if (k_mem_domain_init(&g_domains[ridx], ARRAY_SIZE(base), base) != 0)
			return -1;
#else
		if (k_mem_domain_init(&g_domains[ridx], 0, NULL) != 0)
			return -1;
#endif
		state->initialized = 1;
	}
	k_mem_partition_attr_t expected_rw = K_MEM_PARTITION_P_RW_U_RW;
	if (state->program.start != (uintptr_t)prog_regions[ridx] ||
	    state->program.size != LXP_PROG_REGION_SIZE ||
	    memcmp(&state->program.attr, &expected_rw, sizeof(expected_rw)) != 0 ||
	    state->dynamic.start != (uintptr_t)dyn_pools[ridx] ||
	    state->dynamic.size != LXP_DYN_POOL_SIZE ||
	    memcmp(&state->dynamic.attr, &expected_rw, sizeof(expected_rw)) != 0)
		return -1;
	if (k_mem_domain_add_partition(&g_domains[ridx], &state->program) != 0)
		return -1;
	if (k_mem_domain_add_partition(&g_domains[ridx], &state->dynamic) != 0) {
		k_mem_domain_remove_partition(&g_domains[ridx], &state->program);
		return -1;
	}
	if (state->executable.size != 0u &&
	    k_mem_domain_add_partition(&g_domains[ridx], &state->executable) != 0) {
		k_mem_domain_remove_partition(&g_domains[ridx], &state->dynamic);
		k_mem_domain_remove_partition(&g_domains[ridx], &state->program);
		return -1;
	}
	state->policy = lxp_memory_policy_make_key(&policy);
	state->policy_valid = 1u;
	for (int i = 0; i < LXP_NSLOT; i++)
		if (g_slots[i].policy_valid && g_slots[i].policy.address_space.index == ridx)
			g_slots[i].live_validated = 0u;
	g_slots[sidx].policy = lxp_memory_policy_make_key(&policy);
	g_slots[sidx].policy_valid = 1u;
	g_slots[sidx].live_validated = 0u;
	return 0;
}

static int zephyr_bind_prepared_domain(int sidx, uint32_t generation, int ridx)
{
	lxp_memory_policy_t policy;
	lxp_slot_ref_t slot = {
		.index = (int16_t)sidx,
		.generation = generation,
	};
	if (lxp_slot_memory_policy(slot, &policy) != LXP_OK ||
	    lxp_memory_policy_validate(&policy) != LXP_OK || policy.address_space.index != ridx ||
	    policy.device_count != 0 || !g_regions[ridx].policy_valid ||
	    !lxp_memory_policy_address_space_matches_key(&policy, &g_regions[ridx].policy))
		return -1;
	g_slots[sidx].policy = lxp_memory_policy_make_key(&policy);
	g_slots[sidx].policy_valid = 1u;
	g_slots[sidx].live_validated = 0u;
	return 0;
}

static int zephyr_validate_active_profile(int sidx)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || !g_slots[sidx].policy_valid)
		return 0;
	lxp_slot_ref_t slot = task_slot_ref(sidx);
	const lxp_memory_policy_key_t *key = &g_slots[sidx].policy;
	if (!lxp_slot_ref_equal(key->slot, slot) || key->address_space.index < 0 ||
	    key->address_space.index >= LXP_NREG)
		return 0;
	if (g_slots[sidx].live_validated)
		return 1;

	int ridx = key->address_space.index;
	const struct zephyr_lxp_region *state = &g_regions[ridx];
	if (!state->policy_valid ||
	    !lxp_region_ref_equal(key->address_space, state->policy.address_space) ||
	    key->device_generation != state->policy.device_generation ||
	    key->exec_generation != state->policy.exec_generation ||
	    key->copied_text_base != state->policy.copied_text_base ||
	    key->copied_text_size != state->policy.copied_text_size ||
	    key->copied_text_executable != state->policy.copied_text_executable)
		return 0;
	k_mem_partition_attr_t expected_rw = K_MEM_PARTITION_P_RW_U_RW;
	if (state->program.start != (uintptr_t)prog_regions[ridx] ||
	    state->program.size != LXP_PROG_REGION_SIZE ||
	    memcmp(&state->program.attr, &expected_rw, sizeof(expected_rw)) != 0 ||
	    state->dynamic.start != (uintptr_t)dyn_pools[ridx] ||
	    state->dynamic.size != LXP_DYN_POOL_SIZE ||
	    memcmp(&state->dynamic.attr, &expected_rw, sizeof(expected_rw)) != 0 ||
	    (key->copied_text_executable && (state->executable.start != key->copied_text_base ||
					     state->executable.size != key->copied_text_size)))
		return 0;

#if defined(CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT)
	struct ove_cortex_m_mpu_snapshot snapshot;
	if (ove_cortex_m_mpu_snapshot_read(&snapshot) != 0 ||
	    (snapshot.ctrl & (OVE_CORTEX_M_MPU_CTRL_ENABLE | OVE_CORTEX_M_MPU_CTRL_PRIVDEFENA)) !=
		    (OVE_CORTEX_M_MPU_CTRL_ENABLE | OVE_CORTEX_M_MPU_CTRL_PRIVDEFENA))
		return 0;
	const struct ove_cortex_m_mpu_expectation program = {
		.base = state->program.start,
		.size = state->program.size,
		.texscb = 0x0bu,
		.access = 3u,
		.execute_never = 1u,
	};
	const struct ove_cortex_m_mpu_expectation dynamic = {
		.base = state->dynamic.start,
		.size = state->dynamic.size,
		.texscb = 0x0bu,
		.access = 3u,
		.execute_never = 1u,
	};
	if (!ove_cortex_m_mpu_snapshot_effective_matches(&snapshot, &dynamic))
		return 0;
	if (key->copied_text_executable) {
		const struct ove_cortex_m_mpu_expectation executable = {
			.base = state->executable.start,
			.size = state->executable.size,
			.texscb = 0x0bu,
			.access = 6u,
			.execute_never = 0u,
		};
		const struct ove_cortex_m_mpu_expectation writable_tail = {
			.base = state->executable.start + state->executable.size,
			.size = state->program.start + state->program.size -
				(state->executable.start + state->executable.size),
			.texscb = 0x0bu,
			.access = 3u,
			.execute_never = 1u,
		};
		if (!ove_cortex_m_mpu_snapshot_effective_matches(&snapshot, &executable) ||
		    !ove_cortex_m_mpu_snapshot_effective_contains(&snapshot, &writable_tail))
			return 0;
	} else if (!ove_cortex_m_mpu_snapshot_effective_matches(&snapshot, &program)) {
		return 0;
	}
#endif
	g_slots[sidx].live_validated = 1u;
	return 1;
}

/* ---- the vtable: Zephyr task spawn ----------------------------------------- */
static uint8_t *zephyr_region(int ridx)
{
	return prog_regions[ridx];
}

static uint8_t *zephyr_dyn_pool(int ridx, size_t *size)
{
	if (size)
		*size = LXP_DYN_POOL_SIZE;
	return dyn_pools[ridx];
}

static lxp_exec_capture_t *zephyr_exec_capture(int sidx)
{
	return (sidx >= 0 && sidx < LXP_NSLOT) ? &g_exec_captures[sidx] : NULL;
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static struct ove_cortex_m_cache_geometry g_lxp_cache_geometry;
static const lxp_cpu_memory_contract_t g_lxp_memory_contract =
	OVE_LXP_MEMORY_CONTRACT_STM32F746_INITIALIZER;
#else
static const lxp_cpu_memory_contract_t g_lxp_memory_contract =
	OVE_LXP_MEMORY_CONTRACT_UNCACHED_INITIALIZER;
#endif

static int zephyr_publish_executable(lxp_region_ref_t address_space, uintptr_t base, size_t len)
{
	int ridx = address_space.index;
	if (ridx < 0 || ridx >= LXP_NREG || address_space.generation == 0 || len == 0)
		return LXP_ERR_INVALID_PARAM;
	uintptr_t region_lo = (uintptr_t)prog_regions[ridx];
	if (base != region_lo || len < 32u || len >= LXP_PROG_REGION_SIZE ||
	    (len & (len - 1u)) != 0u)
		return LXP_ERR_INVALID_PARAM;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	if (ove_cortex_m_publish_executable(&g_lxp_cache_geometry, base, len) != 0)
		return LXP_ERR_INVALID_PARAM;
#endif
	return LXP_OK;
}

/* Guest entropy (AT_RANDOM stack-canary seed + getrandom()). sys_csrand_get()
 * propagates entropy-driver failure instead of substituting timer data. */
static int zephyr_random_fill(void *buf, size_t len)
{
	if (!buf && len != 0u)
		return LXP_ERR_INVALID_PARAM;
	return sys_csrand_get(buf, len) == 0 ? LXP_OK : LXP_ERR_BUS_ERROR;
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

static int zephyr_spawn_launch(int sidx, uint32_t generation, int ridx,
			       const lxp_guest_launch_t *launch)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || generation == 0 || !launch || g_slots[sidx].tid)
		return -1;
	if (setup_domain(sidx, generation, ridx) != 0) {
		printk("[lxp] zephyr domain setup failed slot=%d region=%d\n", sidx, ridx);
		return -1;
	}
	/* Reuse the complete-context trampoline for every image. The core owns
	 * FDPIC register semantics; this seam only translates the launch record
	 * into Zephyr's native task entry. */
	struct lxp_resume_ctx *slot =
		(struct lxp_resume_ctx *)((uintptr_t)launch->r[13] - sizeof(struct lxp_resume_ctx));
	lxp_resume_ctx_from_launch(slot, launch);
	g_slots[sidx].tid = k_thread_create(&g_thread_storage[sidx], g_tramp_stacks[sidx],
					    K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]),
					    zephyr_guest_user_enter,
					    (void *)(uintptr_t)launch->r[0], slot, NULL,
					    OVE_ZEPHYR_PRIO_LXP_GUEST, 0, K_FOREVER);
	{ /* Diagnostic task name; CPU attribution uses the native thread identity. */
		char nm[6];
		slot_task_name(nm, sidx);
		k_thread_name_set(g_slots[sidx].tid, nm);
	}
	if (k_mem_domain_add_thread(&g_domains[ridx], g_slots[sidx].tid) != 0) {
		printk("[lxp] zephyr domain bind failed slot=%d region=%d\n", sidx, ridx);
		k_thread_abort(g_slots[sidx].tid);
		g_slots[sidx].tid = NULL;
		return -1;
	}
	g_slots[sidx].generation = generation;
	k_thread_start(g_slots[sidx].tid);
	return 0;
}

static int zephyr_spawn_resume(int sidx, uint32_t generation, int ridx,
			       lxp_spawn_resume_mode_t mode, const struct lxp_resume_ctx *ctx,
			       long r0val)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || generation == 0)
		return -1;
	if (mode == LXP_SPAWN_RESUME_PARKED) {
		if (!g_slots[sidx].tid || g_slots[sidx].generation != generation)
			return -1;
		lxp_memory_policy_t policy;
		if (!g_slots[sidx].policy_valid ||
		    lxp_slot_memory_policy(task_slot_ref(sidx), &policy) != LXP_OK ||
		    !lxp_memory_policy_matches_key(&policy, &g_slots[sidx].policy))
			return -1;
		/* PendSV saved the svc frame that park_frame redirected to the naked
		 * the park entry. Reuse that exact native frame: deriving a new PSP
		 * from the Linux SP loses Zephyr's exception-alignment/lazy-FP
		 * invariants, while resuming through another user exception can try
	 * to preserve stale privileged lazy-FP state under the guest MPU. */
		struct k_thread *thread = &g_thread_storage[sidx];
		struct arch_esf *esf = (struct arch_esf *)(uintptr_t)thread->callee_saved.psp;
		esf->basic.r0 = (uint32_t)r0val;
		esf->basic.r1 = ctx->r1;
		esf->basic.r2 = ctx->r2;
		esf->basic.r3 = ctx->r3;
		esf->basic.r12 = ctx->r12;
		esf->basic.r14 = ctx->lr;
		esf->basic.r15 = ctx->pc & ~1u;
		esf->basic.xpsr = ctx->xpsr | (1u << 24);
		thread->callee_saved.v1 = ctx->r4_11[0];
		thread->callee_saved.v2 = ctx->r4_11[1];
		thread->callee_saved.v3 = ctx->r4_11[2];
		thread->callee_saved.v4 = ctx->r4_11[3];
		thread->callee_saved.v5 = ctx->r4_11[4];
		thread->callee_saved.v6 = ctx->r4_11[5];
		thread->callee_saved.v7 = ctx->r4_11[6];
		thread->callee_saved.v8 = ctx->r4_11[7];
#if LXP_ENABLE_FPU_CONTEXT
		if ((thread->arch.mode_exc_return & (1u << 4)) == 0) {
			for (int i = 0; i < 16; i++)
				esf->fpu.s[i] = ctx->fp.s[i];
			esf->fpu.fpscr = ctx->fp.fpscr;
			memcpy(&thread->arch.preempt_float, &ctx->fp.s[16],
			       sizeof(thread->arch.preempt_float));
		}
#endif
		g_slots[sidx].live_validated = 0u;
		k_thread_resume(g_slots[sidx].tid);
		return 0;
	}
	if (mode != LXP_SPAWN_RESUME_START || g_slots[sidx].tid)
		return -1;
	if (zephyr_bind_prepared_domain(sidx, generation, ridx) != 0)
		return -1;
	/* Stash the resume ctx in the program's OWN user-RW region, just below its resume SP, so
	 * resume_tramp can read it without another MPU partition. AN521 already uses stack plus four
	 * domain partitions; STM32F746 uses stack plus three. A separate shared partition previously
	 * overflowed the AN521 dynamic-region budget and dropped executable kernel text. */
	struct lxp_resume_ctx *slot =
		(struct lxp_resume_ctx *)((uintptr_t)ctx->sp - sizeof(struct lxp_resume_ctx));
	*slot = *ctx;
	g_slots[sidx].tid = k_thread_create(&g_thread_storage[sidx], g_tramp_stacks[sidx],
					    K_THREAD_STACK_SIZEOF(g_tramp_stacks[sidx]),
					    zephyr_guest_user_enter, (void *)r0val, slot, NULL,
					    OVE_ZEPHYR_PRIO_LXP_GUEST, 0, K_FOREVER);
	{ /* Diagnostic task name; CPU attribution uses the native thread identity. */
		char nm[6];
		slot_task_name(nm, sidx);
		k_thread_name_set(g_slots[sidx].tid, nm);
	}
	if (k_mem_domain_add_thread(&g_domains[ridx], g_slots[sidx].tid) != 0) {
		k_thread_abort(g_slots[sidx].tid);
		g_slots[sidx].tid = NULL;
		return -1;
	}
	g_slots[sidx].generation = generation;
	k_thread_start(g_slots[sidx].tid);
	return 0;
}

/* Coordinator critical section: irq_lock masks SVCall (the program svc is an
 * exception, so k_sched_lock would NOT exclude it). Held only for the brief
 * proc-table flag snapshot — never across abort/spawn (which may yield). */
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
static uint32_t g_crit_start_cycles;
#endif
static lxp_critical_token_t zephyr_crit_enter(void)
{
	unsigned int key = irq_lock();
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	g_crit_start_cycles = k_cycle_get_32();
#endif
	return (lxp_critical_token_t)key;
}
static void zephyr_crit_exit(lxp_critical_token_t token)
{
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	uint32_t elapsed = k_cycle_get_32() - g_crit_start_cycles;
#endif
	irq_unlock((unsigned int)token);
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	/* Keep the measurement update outside the interval being measured. */
	critical_metrics_record(elapsed);
#endif
}

/* SCB->ICSR PENDSVSET — raw (0xE000ED04, bit 28), matching the raw-SCS style used elsewhere
 * in the personality seams; avoids a cmsis_core.h include dependency. Writing the whole word
 * is the documented idiom (the other writable ICSR bits are write-1-to-act, so writing 0 to
 * them is a no-op) — Zephyr's own z_arm_exc_exit does `SCB->ICSR = SCB_ICSR_PENDSVSET_Msk`. */
#define LXP_ICSR (*(volatile uint32_t *)0xE000ED04u)
#define LXP_PENDSVSET (1u << 28)
#define LXP_CFSR (*(volatile uint32_t *)0xE000ED28u)
#define LXP_HFSR (*(volatile uint32_t *)0xE000ED2Cu)
#define LXP_MMFAR (*(volatile uint32_t *)0xE000ED34u)
#define LXP_BFAR (*(volatile uint32_t *)0xE000ED38u)

/* Event wakeup: the dispatch (fault/exception context) gives this when a program parks; the
 * coordinator takes it instead of busy-polling. ISR-safe k_sem_give. */
K_SEM_DEFINE(g_lxp_ev, 0, 1);
static void zephyr_event_post(void)
{
	k_sem_give(&g_lxp_ev);
	/* The give readies the higher-priority coordinator, but a program svc reaches us via the
	 * kernel-oops path (svc.S .L_oops returns with `pop {r0,pc}`, bypassing z_arm_int_exit),
	 * so nothing pends PendSV — the just-parked K_USER program keeps busy-spinning in
	 * the park entry until its timeslice expires (~tens of ms), which is the entire cause
	 * of the multi-ms pipe/spawn latency. Pend PendSV ourselves so the coordinator is switched
	 * in on exception return, exactly as z_arm_exc_exit would for a real ISR. A rare no-op
	 * self-switch (nothing higher became ready) is harmless. In thread context (the
	 * coordinator's own cross-kill post) k_sem_give already reschedules, so skip. */
	if (k_is_in_isr()) {
		LXP_ICSR = LXP_PENDSVSET;
	}
}
static void zephyr_event_wait(unsigned ms)
{
	k_sem_take(&g_lxp_ev, K_MSEC(ms));
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
struct zephyr_lnx_fault_diag {
	uint32_t count;
	uint32_t reason;
	uint32_t cfsr;
	uint32_t hfsr;
	uint32_t mmfar;
	uint32_t bfar;
	uint32_t pc;
	uint32_t suppressed_dump_lines;
};
/* Host-SRAM, non-static post-mortem record. Fault context only copies registers;
 * formatting remains in the coordinator's bounded on_guest_exit callback. */
volatile struct zephyr_lnx_fault_diag g_zephyr_lxp_fault_diag[LXP_NSLOT];
static uint32_t g_guest_fault_dump_consumed;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	if (lxp_trap_active()) {
		int sidx = current_slot();
		if (sidx >= 0) {
			volatile struct zephyr_lnx_fault_diag *diag =
				&g_zephyr_lxp_fault_diag[sidx];
			diag->count++;
			diag->reason = reason;
			diag->cfsr = LXP_CFSR & 0x03ffffffu;
			diag->hfsr = LXP_HFSR;
			diag->mmfar = LXP_MMFAR;
			diag->bfar = LXP_BFAR;
			diag->pc = esf ? esf->basic.pc : 0u;
			diag->suppressed_dump_lines =
				g_guest_fault_dump_lines - g_guest_fault_dump_consumed;
			g_guest_fault_dump_consumed = g_guest_fault_dump_lines;

			lxp_guest_fault_t fault = {
				.detail = diag->cfsr ? diag->cfsr : reason,
				.address = (diag->cfsr & (1u << 7))    ? diag->mmfar
					   : (diag->cfsr & (1u << 15)) ? diag->bfar
								       : 0u,
			};
			(void)lxp_slot_report_memory_fault(task_slot_ref(sidx), &fault);
			return;
		}
	}
	k_fatal_halt(reason);
}

static int zephyr_abort_slot(int sidx, uint32_t generation)
{
	if (sidx < 0 || sidx >= LXP_NSLOT)
		return -1;
	if (g_slots[sidx].tid && g_slots[sidx].generation != generation)
		return -1;
	if (g_slots[sidx].tid)
		k_thread_abort(g_slots[sidx].tid);
	g_slots[sidx].tid = NULL;
	g_slots[sidx].generation = 0;
	g_slots[sidx].policy_valid = 0;
	g_slots[sidx].live_validated = 0;
	return 0;
}

static int zephyr_park_slot(int sidx, uint32_t generation)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || !lxp_slot_ref_is_runnable(task_slot_ref(sidx)) ||
	    !g_slots[sidx].tid || g_slots[sidx].generation != generation)
		return -1;
	k_thread_suspend(g_slots[sidx].tid);
	return 0;
}

static int32_t slot_for_thread(uintptr_t identity)
{
	for (int s = 0; s < LXP_NSLOT; s++)
		if (identity == (uintptr_t)g_slots[s].tid)
			return s;
	return LXP_THREAD_SLOT_NONE;
}

static int lxp_seam_thread_list(struct lxp_thread_info *out, size_t max_count, size_t *actual_count)
{
	return lxp_ove_thread_snapshot_read(&g_thread_snapshot, out, max_count, actual_count,
					    slot_for_thread);
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
	"Zephyr " KERNEL_VERSION_STRING " ove-" OVE_BUILD_OVERTOS_REV " lxp-" OVE_BUILD_LXP_REV
_Static_assert(sizeof(LXP_SYSTEM_VERSION) <= 65u, "uname version exceeds Linux utsname field");
static const char *lxp_seam_system_version(void)
{
	return LXP_SYSTEM_VERSION;
}

static int zephyr_prepare(void)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	if (ove_cortex_m_cache_geometry_read(&g_lxp_cache_geometry) != 0)
		return LXP_ERR_INVALID_PARAM;
#endif
	for (int r = 0; r < LXP_NREG; r++)
		g_regions[r].policy_valid = 0;
	for (int s = 0; s < LXP_NSLOT; s++)
		g_slots[s].policy_valid = 0;
	return LXP_OK;
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static int zephyr_validate_static_mpu(void)
{
	struct ove_cortex_m_mpu_snapshot snapshot;
	if (ove_cortex_m_mpu_snapshot_read(&snapshot) != 0 || snapshot.count != 8u ||
	    (snapshot.ctrl & (OVE_CORTEX_M_MPU_CTRL_ENABLE | OVE_CORTEX_M_MPU_CTRL_PRIVDEFENA)) !=
		    (OVE_CORTEX_M_MPU_CTRL_ENABLE | OVE_CORTEX_M_MPU_CTRL_PRIVDEFENA))
		return 0;

	const struct ove_cortex_m_mpu_region *sdram = NULL;
	for (unsigned i = 0; i < snapshot.count; i++)
		if (ove_cortex_m_mpu_region_matches(&snapshot.regions[i], 0xc0000000u,
						    8u * 1024u * 1024u, 0u, 0x0bu, 1u, 1u)) {
			if (sdram)
				return 0;
			sdram = &snapshot.regions[i];
		}
	if (!sdram || !ove_cortex_m_mpu_region_contains(sdram, (uintptr_t)&g_lxp_ext_storage,
							sizeof(g_lxp_ext_storage)))
		return 0;
#if defined(CONFIG_OVE_FB)
	uintptr_t framebuffer = (uintptr_t)ove_hal_fb_buffer();
	uintptr_t storage = (uintptr_t)&g_lxp_ext_storage;
	size_t framebuffer_size = 480u * 272u * 2u;
	if (framebuffer == 0u ||
	    !ove_cortex_m_mpu_region_contains(sdram, framebuffer, framebuffer_size) ||
	    !(storage + sizeof(g_lxp_ext_storage) <= framebuffer ||
	      framebuffer + framebuffer_size <= storage))
		return 0;
#endif
	return 1;
}
#endif

static int zephyr_validate_memory_contract(const lxp_cpu_memory_contract_t *declared)
{
	if (declared != &g_lxp_memory_contract)
		return LXP_ERR_INVALID_PARAM;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	return ove_lxp_memory_contract_matches_cache(declared, &g_lxp_cache_geometry) &&
			       zephyr_validate_static_mpu()
		       ? LXP_OK
		       : LXP_ERR_INVALID_PARAM;
#else
	return (OVE_SCB_CCR & OVE_SCB_CCR_DC) == 0u ? LXP_OK : LXP_ERR_INVALID_PARAM;
#endif
}

const lxp_os_ops_t g_lxp_host_engine = {
	.abi_version = LXP_OS_OPS_ABI_VERSION,
	.struct_size = sizeof(lxp_os_ops_t),
	.prepare = zephyr_prepare,
	.region = zephyr_region,
	.dyn_pool = zephyr_dyn_pool,
	.exec_capture = zephyr_exec_capture,
	.random_fill = zephyr_random_fill,
	.spawn_launch = zephyr_spawn_launch,
	.spawn_resume = zephyr_spawn_resume,
	.abort_slot = zephyr_abort_slot,
	.park_entry = zephyr_park_entry,
	.park_prepare = zephyr_park_prepare,
	.park_slot = zephyr_park_slot,
	.crit_enter = zephyr_crit_enter,
	.crit_exit = zephyr_crit_exit,
	.event_post = zephyr_event_post,
	.event_wait = zephyr_event_wait,
	/* OS-service ops (host adapter). */
	.time_us = ove_time_get_us,
	.time_ns = ove_time_get_ns,
	.thread_list = lxp_seam_thread_list,
	.mem_stats = lxp_seam_mem_stats,
	.system_version = lxp_seam_system_version,
	.publish_executable = zephyr_publish_executable,
	.cpu_memory_contract = &g_lxp_memory_contract,
	.validate_memory_contract = zephyr_validate_memory_contract,
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	.exec_stage = zephyr_exec_stage,
#endif
};

/* The public lxp_run() now lives in the module (src/lxp_run.c). */
