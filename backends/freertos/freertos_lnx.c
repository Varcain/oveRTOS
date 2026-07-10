/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * FreeRTOS seam for the Linux personality. The engine-agnostic run loop, svc
 * dispatch, and signal delivery live in backends/common/ove_lnx_run.c; this file
 * supplies only the FreeRTOS-specific bits: the svc trap, the program memory,
 * and the task spawn (via the ove_lnx_engine vtable).
 *
 * PHASE 1 (functional parity): the program runs as a normal PRIVILEGED FreeRTOS
 * task on the non-MPU ARM_CM7 port. Its `svc #0` takes the SVCall exception,
 * which this seam OWNS: the board's FreeRTOSConfig.h does NOT alias
 * vPortSVCHandler->SVC_Handler, so the strong SVC_Handler below is the vector; it
 * dispatches the program's svc (while a run is active) to the personality and
 * forwards FreeRTOS's own start-scheduler svc to vPortSVCHandler. Phase 2 will
 * switch to the ARM_CM4_MPU port for unprivileged + MPU isolation.
 */

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <string.h>

#include "../common/ove_lnx_run.h"
#include "ove/linux/syscall.h" /* ove_lnx_rootfs_window — strong-overridden below for QSPI-XIP */

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#include "stm32f7xx.h" /* SCB_CleanDCache / SCB_InvalidateICache: M7 loaded-code coherency */
#endif

#define TRAMP_STACK_WORDS 256u		  /* tramp prologue; the program uses its own stack */
#define SLOT_PRIO (tskIDLE_PRIORITY + 1u) /* below the run-loop task (its creator) */

/* Under the ARM_CM4_MPU port the task's privilege rides in the top bit of its priority
 * (portPRIVILEGE_BIT); on the non-MPU port the symbol is undefined → 0 (a no-op, all
 * tasks privileged). PHASE A keeps the Linux program PRIVILEGED so the MPU port is
 * exercised before the isolation change; PHASE B drops the bit + spawns it restricted. */
#ifndef portPRIVILEGE_BIT
#define portPRIVILEGE_BIT 0u
#endif
#define SLOT_PROG_PRIO (SLOT_PRIO | portPRIVILEGE_BIT)

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Real STM32F746 hardware: the MCU has only 320K of internal SRAM — far too small for the 2M
 * region pool + 1M dyn pools — so both live in the board's 8M external SDRAM (0xC0000000) via
 * the linker's .sdram_bss (NOLOAD) section. The board (bsp.c) brings up the FMC controller and
 * makes the SDRAM region executable + Normal non-cacheable (the latter keeps loaded/relocated
 * program code coherent on the M7 with no SCB cache maintenance) before the run loop runs. */
/* Phase-2 MPU isolation: the program runs UNPRIVILEGED with a per-task MPU region over its
 * program region + dyn_pool, and PMSAv7 requires each region's base aligned to its power-of-2
 * size — so both arrays are size-aligned (not just 32B) within .sdram_bss. */
static uint8_t prog_regions[OVE_LNX_NREG][OVE_LNX_PROG_REGION_SIZE]
	__attribute__((section(".sdram_bss"), aligned(OVE_LNX_PROG_REGION_SIZE)));
static uint8_t dyn_pools[OVE_LNX_NREG][OVE_LNX_DYN_POOL_SIZE]
	__attribute__((section(".sdram_bss"), aligned(OVE_LNX_DYN_POOL_SIZE)));
#else
/* Both pools live in PSRAM (0x60000000, 16M; NOLOAD → no flash cost). Phase-2 MPU isolation:
 * the program runs UNPRIVILEGED with a per-task MPU region over its program region + dyn_pool,
 * and PMSAv7 requires each region's base to be aligned to its (power-of-2) size — so both arrays
 * are size-aligned. PSRAM also keeps them off the kernel's 4M SRAM (the dynamic FDPIC proc's
 * arena anyway needs room to mmap libc.so ~500K, far past the in-region 96K arena). */
static uint8_t prog_regions[OVE_LNX_NREG][OVE_LNX_PROG_REGION_SIZE]
	__attribute__((section(".psram"), aligned(OVE_LNX_PROG_REGION_SIZE)));
static uint8_t dyn_pools[OVE_LNX_NREG][OVE_LNX_DYN_POOL_SIZE]
	__attribute__((section(".psram"), aligned(OVE_LNX_DYN_POOL_SIZE)));
#endif
static StaticTask_t g_tcb[OVE_LNX_NSLOT];
static TaskHandle_t g_tid[OVE_LNX_NSLOT];
/* The tramp/program stacks are the restricted task's auto MPU stack region, so each must be
 * aligned to its (power-of-2) size (PMSAv7). 256 words = 1 KB. */
static StackType_t g_tramp_stacks[OVE_LNX_NSLOT][TRAMP_STACK_WORDS]
	__attribute__((aligned(TRAMP_STACK_WORDS * sizeof(StackType_t))));

static int current_slot(void)
{
	/* Read pxCurrentTCB directly instead of xTaskGetCurrentTaskHandle(): under the MPU port the
	 * accessor is an MPU_* wrapper that does a privilege check + trampoline on every call, and it
	 * svc-raises-privilege when the caller looks unprivileged — which HardFaults from the svc-handler
	 * context (the reason e70fc5f switched the tick sampler to the raw read too). The handle IS the
	 * TCB pointer, and handler mode reads privileged data fine. Removes the wrapper indirection from
	 * every syscall's slot lookup. */
	extern void *volatile pxCurrentTCB;
	TaskHandle_t t = (TaskHandle_t)pxCurrentTCB;
	for (int i = 0; i < OVE_LNX_NSLOT; i++)
		if (g_ove_lnx_used[i] && g_tid[i] == t)
			return i;
	return -1;
}

/* ---- the SVC trap ---------------------------------------------------------- */
/* The HW-stacked exception frame (live on the program PSP) + the callee-saved
 * registers the asm shim captures before they are clobbered. */
struct lnx_capture {
	uint32_t *hw;	   /* hw[0..7] = r0,r1,r2,r3,r12,lr,pc,xpsr */
	uint32_t psp;	   /* the program SP at the HW frame */
	uint32_t r4_11[8]; /* r4..r11 */
};
static struct lnx_capture g_cap __attribute__((used)); /* referenced from SVC_Handler asm */

/* The C body of the svc trap: build the uniform frame, dispatch, write back.
 * Returns 1 if it handled a program's svc #0 (Linux syscall), 0 to FORWARD the svc
 * to FreeRTOS. Under the MPU port FreeRTOS uses svc itself (portYIELD = svc #101,
 * raised by the privileged idle task; raise-privilege = svc #102; start-scheduler =
 * svc #100), so any svc from a non-program task (current_slot() < 0) must reach
 * vPortSVCHandler. The current_slot() gate also blocks escalation: a malicious svc
 * #102 from a program task is current_slot() >= 0 → dispatched as a (bogus) syscall,
 * never reaching the port's raise-privilege path. */
int freertos_lnx_svc_c(struct lnx_capture *g)
{
	int sidx = current_slot();
	if (sidx < 0)
		return 0; /* not a program task → forward to FreeRTOS */
	struct ove_lnx_frame f;
	f.r[0] = g->hw[0];
	f.r[1] = g->hw[1];
	f.r[2] = g->hw[2];
	f.r[3] = g->hw[3];
	for (int i = 0; i < 8; i++)
		f.r[4 + i] = g->r4_11[i];
	f.r[12] = g->hw[4];
	/* The HW frame is 32 bytes; the pre-svc SP is +32 (+4 if xPSR aligned). */
	f.r[13] = g->psp + 32u + ((g->hw[7] & (1u << 9)) ? 4u : 0u);
	f.r[14] = g->hw[5];
	f.r[15] = g->hw[6];
	f.xpsr = g->hw[7];

	ove_lnx_dispatch(&f, &g_ove_lnx_proc[sidx]);

	g->hw[0] = f.r[0];
	g->hw[1] = f.r[1];
	g->hw[2] = f.r[2];
	g->hw[3] = f.r[3];
	g->hw[4] = f.r[12];
	g->hw[5] = f.r[14];
	g->hw[6] = f.r[15];
	g->hw[7] = f.xpsr;
	return 1;
}

extern void vPortSVCHandler(void); /* FreeRTOS's own (start-scheduler / yield / priv) handler */

/* SVC vector: while a run is active, capture the frame + dispatch the program's
 * svc; otherwise forward to FreeRTOS (start scheduler). */
__attribute__((naked)) void SVC_Handler(void)
{
	__asm__ volatile("ldr   r1, =g_ove_lnx_active \n"
			 "ldr   r1, [r1]              \n"
			 "cmp   r1, #0                \n"
			 "beq   1f                    \n" /* inactive -> FreeRTOS */
			 /* EXC_RETURN bit 3 == 0 -> the svc was taken from HANDLER mode.  A Linux program
			  * always syscalls from THREAD mode (it runs as an unprivileged thread-mode task),
			  * so a handler-mode svc is never a program syscall — it is FreeRTOS's own (yield /
			  * raise-privilege / start-scheduler).  Capturing it from PSP (stale program frame)
			  * + dispatching it as a syscall is what corrupted the post-exit context.  Forward. */
			 "tst   lr, #8                \n"
			 "beq   1f                    \n"
			 "mrs   r0, psp               \n" /* r0 = HW exception frame */
			 "ldr   r1, =g_cap            \n"
			 "str   r0, [r1, #0]          \n" /* g_cap.hw  */
			 "str   r0, [r1, #4]          \n" /* g_cap.psp */
			 "add   r2, r1, #8            \n"
			 "stmia r2, {r4-r11}          \n" /* g_cap.r4_11 */
			 "mov   r0, r1                \n"
			 /* The dispatch runs in HANDLER mode but inherits the program's CONTROL.nPRIV=1.
			  * FreeRTOS MPU_* wrappers (e.g. the event_post semaphore-give on a parking syscall)
			  * read nPRIV, believe they're unprivileged, and raise-privilege via svc #102 — taken
			  * inside this active SVCall it escalates to a HardFault. Clear nPRIV across the
			  * dispatch (handler mode is privileged regardless), then restore so the program
			  * resumes UNPRIVILEGED. */
			 "mrs   r2, control           \n"
			 "push  {r2, lr}              \n"
			 "bic   r3, r2, #1            \n"
			 "msr   control, r3           \n"
			 "isb                         \n"
			 "bl    freertos_lnx_svc_c    \n"
			 "pop   {r2, lr}              \n"
			 "msr   control, r2           \n"
			 "isb                         \n"
			 "cmp   r0, #0                \n"
			 "beq   1f                    \n" /* 0 = not a program svc -> forward */
			 "bx    lr                    \n" /* 1 = handled: exception return, replay frame */
			 "1:                          \n"
			 "b     vPortSVCHandler       \n");
}

/* ---- MemManage fault containment ------------------------------------------- */
/* An UNPRIVILEGED program making an illegal access raises MemManage (enabled by the MPU port's
 * prvSetupMPU). Contain it like a default-action SIGSEGV: mark the proc exited (139), redirect its
 * stacked return PC to the park loop, wake the coordinator (which reaps the slot + frees the region
 * via EV_EXIT) and exception-return — the kernel and other programs are untouched. A fault from any
 * NON-program (privileged kernel) context is a real bug → fatal. Strong symbol overriding the weak
 * MemManage_Handler in the CMSIS startup. */
static void freertos_event_post(void); /* forward decl; defined with the vtable below */
extern void HardFault_Handler(void);

void freertos_lnx_memfault_c(uint32_t *frame /* the faulting program's PSP HW frame */)
{
	int sidx = current_slot();
	if (g_ove_lnx_active && sidx >= 0) {
		g_ove_lnx_proc[sidx].exited = 1;
		g_ove_lnx_proc[sidx].exit_status = 139;		 /* 128 + SIGSEGV */
		frame[6] = ((uint32_t)&ove_lnx_park_loop) & ~1u; /* stacked PC -> park loop */
		frame[7] |= (1u << 24);				 /* xPSR.T (Thumb) */
		*(volatile uint32_t *)0xE000ED28 =
			*(volatile uint32_t *)0xE000ED28; /* clear MMFSR (write-1-to-clear) */
		freertos_event_post();
		return;
	}
	HardFault_Handler(); /* not a program fault -> fatal */
}

__attribute__((naked)) void MemManage_Handler(void)
{
	__asm__ volatile("mrs  r0, psp                \n" /* r0 = faulting program's HW frame */
			 /* Same nPRIV dance as SVC_Handler: memfault_c calls FreeRTOS APIs
			  * (event_post) which must not raise-privilege via svc from here. */
			 "mrs  r2, control            \n"
			 "push {r2, lr}               \n"
			 "bic  r3, r2, #1             \n"
			 "msr  control, r3            \n"
			 "isb                         \n"
			 "bl   freertos_lnx_memfault_c\n"
			 "pop  {r2, lr}               \n"
			 "msr  control, r2            \n"
			 "isb                         \n"
			 "bx   lr                     \n");
}

/* UsageFault (undefined instruction / bad control flow) + BusFault get the SAME containment as
 * MemManage: a program fault is killed (139), a kernel fault is fatal. Both tail-branch into
 * MemManage_Handler's body (which reads the faulting PSP frame + calls freertos_lnx_memfault_c —
 * fault-type-agnostic, and its CFSR write-back already clears BFSR/UFSR too). Enabled via SHCSR in
 * ove_lnx_run; the MPU port's prvSetupMPU only turns on MEMFAULTENA. */
__attribute__((naked)) void UsageFault_Handler(void)
{
	__asm__ volatile("b MemManage_Handler");
}
__attribute__((naked)) void BusFault_Handler(void)
{
	__asm__ volatile("b MemManage_Handler");
}

/* ---- thread entry: in-region descriptor + unified trampoline --------------- */
/* The program's entry/resume context is stashed in the program's OWN region (just below SP)
 * as a resume_desc, so the program task's (possibly UNPRIVILEGED) trampoline can read it without
 * touching kernel memory, and the privileged coordinator writes it via background access. */
struct resume_desc {
	uint32_t r0;
	struct ove_lnx_resume_ctx ctx; /* r4_11[8], r12, lr, sp, pc */
};

__attribute__((naked)) static void prog_tramp(void *desc __attribute__((unused)))
{
	/* desc in r0. Restore r4..r11 (r7/r8/r9 = FDPIC exec-loadmap / interp-loadmap / GOT), r12,
	 * lr, then r0 = desc->r0, switch SP last (it clobbers sp), and branch to ctx.pc. */
	__asm__ volatile("add   r1, r0, #4    \n" /* r1 -> ctx */
			 "ldmia r1!, {r4-r11}\n"
			 "ldr   r12, [r1], #4\n"
			 "ldr   lr,  [r1], #4\n"
			 "ldr   r2,  [r1], #4\n" /* ctx.sp */
			 "ldr   r3,  [r1]    \n" /* ctx.pc */
			 "ldr   r0,  [r0]    \n" /* desc->r0 */
			 "mov   sp,  r2      \n"
			 "bx    r3           \n");
}

/* Bytes the descriptor occupies, rounded up to whole 32-byte D-cache lines. */
#define RESUME_DESC_CLSPAN (((uint32_t)sizeof(struct resume_desc) + 31u) & ~31u)

/* Stash the descriptor just below the program SP, INSIDE the program region (xRegions[0]).
 * Cache-line (32B) aligned and rounded to whole lines that sit ENTIRELY below sp's line: with the
 * D-cache on the coordinator writes this through its uncached (Device) view but the guest's
 * prog_tramp reads it cacheable, so freertos_spawn_resume must invalidate its lines — and that
 * invalidate must not clip the parent's live stack at/above sp (which may hold dirty, not-yet
 * written-back data the resuming child still needs). */
static struct resume_desc *stash_desc(uint32_t sp, const struct ove_lnx_resume_ctx *ctx, long r0)
{
	struct resume_desc *d = (struct resume_desc *)((sp & ~31u) - RESUME_DESC_CLSPAN);
	d->r0 = (uint32_t)r0;
	d->ctx = *ctx;
	return d;
}

/* Spawn the program task entering prog_tramp. MPU build: a RESTRICTED, UNPRIVILEGED task whose
 * only RW regions are its program region + dyn_pool (both execute-never; code runs from the flash
 * cpio via the static unprivileged-RX flash region — clean W^X). Non-MPU build (e.g. STM32): a
 * plain privileged task. */
static int freertos_spawn_common(int sidx, int ridx, struct resume_desc *desc)
{
	char nm[5] = {'l', 'n', 'x', (char)('0' + sidx), 0}; /* per-slot: ps/top per-proc CPU */
#if (portUSING_MPU_WRAPPERS == 1)
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	/* The program region + dyn_pool live in external SDRAM.  Make them Normal WBWA CACHEABLE,
	 * non-shareable (0x0B): LVGL's malloc'd draw buffer lands in the program region's heap, so
	 * caching the per-pixel compositing writes is the large lvbench win on the memory-bound scenes
	 * (they were writing every pixel straight to uncached FMC SDRAM).  NON-shareable is required —
	 * the port default 0x07 is *shareable*, which precise-BusFaults the M7's FMC accesses on real
	 * silicon.  Coherency: the loader writes each program's image to SDRAM through the coordinator's
	 * uncached (Device background) view, so freertos_spawn_launch INVALIDATES this region's D-cache
	 * before the guest runs (drop stale lines from the previous tenant of this ridx → the guest
	 * fills the fresh image from SDRAM); the LTDC framebuffer at 0xC0000000 stays non-cacheable
	 * (bsp region 0), and the guest blits its cacheable draw buffer to it coherently on the same
	 * core.  Mirrors the NuttX backend (nuttx_lnx_trap.c set_prog_regions, also 0x0B). */
	const uint32_t tex_s_c_b = 0x0Bu; /* TEX=001, S=0, C=1, B=1 — Normal WBWA cacheable, non-shareable */
#else
	const uint32_t tex_s_c_b = configTEX_S_C_B_SRAM; /* an500 PSRAM: port default 0x07 */
#endif
	const uint32_t rw_xn = portMPU_REGION_READ_WRITE | portMPU_REGION_EXECUTE_NEVER |
			       (tex_s_c_b << portMPU_RASR_TEX_S_C_B_LOCATION);
	/* pxTaskBuffer is `StaticTask_t * const`, so a designated initializer is required (it also
	 * zeroes the remaining configurable region xRegions[2]). xRegions[0] = the program region,
	 * xRegions[1] = the dyn_pool — both RW + execute-never (W^X; code runs from the flash cpio). */
	TaskParameters_t tp = {
		.pvTaskCode = prog_tramp,
		.pcName = nm,
		.usStackDepth = TRAMP_STACK_WORDS,
		.pvParameters = desc,
		.uxPriority = SLOT_PRIO, /* NO portPRIVILEGE_BIT -> UNPRIVILEGED */
		.puxStackBuffer = g_tramp_stacks[sidx],
		.pxTaskBuffer = &g_tcb[sidx],
		.xRegions = {
			{prog_regions[ridx], OVE_LNX_PROG_REGION_SIZE, rw_xn},
			{dyn_pools[ridx], OVE_LNX_DYN_POOL_SIZE, rw_xn},
		},
	};
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	/* The guest XIPs its FDPIC text in-place from the rootfs cpio in the on-board QSPI NOR
	 * (memory-mapped at 0x90000000). The static unprivileged-flash MPU region stays on internal
	 * flash — that is where prog_tramp / ove_lnx_park_loop / the resume stubs live, which this
	 * unprivileged task must execute before and between running its own code — so the QSPI XIP
	 * window needs its own per-task region. Unprivileged RO + executable (W^X: the guest never
	 * writes QSPI; its RW data/stack are the SDRAM regions above). Normal-cacheable so the M7's
	 * I-cache absorbs the slow external-flash fetches. 16 MB is the whole NOR window (16 MB-aligned
	 * base → a single valid PMSAv7 region). Uses xRegions[2] = MPU region 2, the last free
	 * configurable slot (portNUM_CONFIGURABLE_REGIONS == 3 at configTOTAL_MPU_REGIONS == 8). */
	tp.xRegions[2].pvBaseAddress = (void *)0x90000000u;
	tp.xRegions[2].ulLengthInBytes = 16u * 1024u * 1024u;
	/* 0x02 = Normal WRITE-THROUGH, NON-shareable → D-CACHEABLE.  NOT the port default
	 * configTEX_S_C_B_FLASH (0x07), which is SHAREABLE: the single-core M7 has no coherency unit, so
	 * it treats shareable Normal memory as NON-cacheable for the D-cache — meaning the guest reads its
	 * fonts / images / rodata straight from the slow QSPI NOR on every pixel, the dominant render cost
	 * (the D-cache is "on" but the biggest read source bypasses it → render no better than uncached).
	 * Non-shareable lets the D-cache absorb those reads (RO XIP window → no coherency concern), and the
	 * I-cache still covers code fetch.  Matches NuttX region 4 (0x02): lvbench render 108 -> 65 ms,
	 * 12 -> 16 FPS — closes the ~2x gap to NuttX.  (Write-through vs write-back is moot for a RO
	 * window.) */
	tp.xRegions[2].ulParameters =
		portMPU_REGION_READ_ONLY | (0x02u << portMPU_RASR_TEX_S_C_B_LOCATION);
#endif
	BaseType_t ok = xTaskCreateRestrictedStatic(&tp, &g_tid[sidx]);
	g_ove_lnx_used[sidx] = (ok == pdPASS);
	return (ok == pdPASS) ? 0 : -1;
#else
	(void)ridx;
	g_tid[sidx] = xTaskCreateStatic(prog_tramp, nm, TRAMP_STACK_WORDS, desc, SLOT_PROG_PRIO,
					g_tramp_stacks[sidx], &g_tcb[sidx]);
	g_ove_lnx_used[sidx] = (g_tid[sidx] != NULL);
	return g_tid[sidx] ? 0 : -1;
#endif
}

/* ---- the vtable: FreeRTOS task spawn --------------------------------------- */
static uint8_t *freertos_region(int ridx)
{
	return prog_regions[ridx];
}

static uint8_t *freertos_dyn_pool(int ridx, size_t *size)
{
	if (size)
		*size = OVE_LNX_DYN_POOL_SIZE;
	return dyn_pools[ridx];
}

static int freertos_spawn_launch(int sidx, int ridx, const ove_flat_t *prog, void *entry, void *sp,
				 void *stack_lo)
{
	(void)stack_lo;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	/* The loader wrote this program's image (data + relocations) to the SDRAM program region
	 * through the coordinator's uncached (Device background) view, and materialised new code paths.
	 * With the D-cache ON the region is Normal WBWA cacheable (freertos_spawn_common), so INVALIDATE
	 * exactly this region before the guest runs: drop stale cacheable lines from the previous tenant
	 * of this ridx so the guest's first reads miss + fill the fresh image from SDRAM.  Invalidate —
	 * NOT clean — a clean would write those stale lines back OVER the loader's fresh SDRAM.  (D-cache
	 * off: no lines to drop; skip the walk.)  Then invalidate the I-cache so the CPU fetches the real
	 * code rather than whatever was physically in SDRAM. */
	if (SCB->CCR & SCB_CCR_DC_Msk)
		SCB_InvalidateDCache_by_Addr((void *)prog_regions[ridx], (int32_t)OVE_LNX_PROG_REGION_SIZE);
	SCB_InvalidateICache();
	__DSB();
	__ISB();
#endif
	/* FDPIC entry: r7 = exec loadmap, r8 = interp (ld.so) loadmap, r9 = GOT (r4_11[3..5]);
	 * r4/5/6/10/11/r12/lr = 0 (the crt _start sets them up); r0 = 0 (static fini = NULL); pc = entry. */
	struct ove_lnx_resume_ctx c;
	memset(&c, 0, sizeof(c));
	c.r4_11[3] = prog->is_fdpic ? (uint32_t)prog->loadmap : 0u;
	c.r4_11[4] = prog->is_fdpic ? (uint32_t)prog->interp_loadmap : 0u;
	c.r4_11[5] = prog->is_fdpic ? (uint32_t)prog->got : 0u;
	c.sp = (uint32_t)sp;
	c.pc = (uint32_t)entry;
	return freertos_spawn_common(sidx, ridx, stash_desc((uint32_t)sp, &c, 0));
}

static void freertos_spawn_resume(int sidx, int ridx, const struct ove_lnx_resume_ctx *ctx,
				  long r0val)
{
	struct resume_desc *d = stash_desc(ctx->sp, ctx, r0val);
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	/* Unlike a launch (freertos_spawn_launch invalidates the whole freshly-loaded region), a resume
	 * keeps the region's LIVE guest data — so invalidate only the descriptor's own cache lines.
	 * stash_desc wrote it through the coordinator's uncached (Device background) view; prog_tramp
	 * reads it cacheable, so drop any stale line covering it → that read fills fresh from SDRAM.
	 * The lines sit below sp (stash_desc), so no live parent stack is clipped. */
	if (SCB->CCR & SCB_CCR_DC_Msk)
		SCB_InvalidateDCache_by_Addr((void *)d, (int32_t)RESUME_DESC_CLSPAN);
#endif
	(void)freertos_spawn_common(sidx, ridx, d);
}

static void freertos_abort_slot(int sidx)
{
	if (g_ove_lnx_used[sidx] && g_tid[sidx])
		vTaskDelete(g_tid[sidx]);
	g_ove_lnx_used[sidx] = 0;
	g_tid[sidx] = NULL;
}

static void freertos_sleep_ms(unsigned ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

/* Coordinator critical section: taskENTER_CRITICAL raises BASEPRI to mask the
 * configurable-priority interrupts (NOT vTaskSuspendAll, which only defers thread
 * switches). Held only for the brief proc-table flag snapshot. */
static void freertos_crit_enter(void)
{
	taskENTER_CRITICAL();
}
static void freertos_crit_exit(void)
{
	taskEXIT_CRITICAL();
}

/* Event wakeup: the coordinator blocks here instead of busy-polling; the dispatch
 * (SVC exception context) gives the binary semaphore when a program parks. */
static StaticSemaphore_t g_ev_buf;
static SemaphoreHandle_t g_ev;
static void freertos_event_post(void)
{
	BaseType_t woken = pdFALSE;
	if (g_ev)
		xSemaphoreGiveFromISR(g_ev, &woken);
	/* Pend a context switch if the (higher-priority) coordinator was woken: event_post runs
	 * from the SVC/MemManage handler (e.g. a program's exit park_frame). Without this yield the
	 * woken coordinator does NOT preempt, so an EXITED unprivileged restricted task keeps spinning
	 * in ove_lnx_park_loop and is context-switched (corrupting its saved registers under the MPU
	 * port) before the coordinator reaps it. Yielding reaps it promptly, before any such switch. */
	portYIELD_FROM_ISR(woken);
}
static void freertos_event_wait(unsigned ms)
{
	if (g_ev)
		xSemaphoreTake(g_ev, pdMS_TO_TICKS(ms));
}

static const struct ove_lnx_engine g_freertos_engine = {
	.region = freertos_region,
	.dyn_pool = freertos_dyn_pool,
	.spawn_launch = freertos_spawn_launch,
	.spawn_resume = freertos_spawn_resume,
	.abort_slot = freertos_abort_slot,
	.sleep_ms = freertos_sleep_ms,
	.crit_enter = freertos_crit_enter,
	.crit_exit = freertos_crit_exit,
	.event_post = freertos_event_post,
	.event_wait = freertos_event_wait,
};

#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI) && defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && \
	(portUSING_MPU_WRAPPERS == 1)
/* Strong override of the engine-common weak no-op (backends/common/ove_lnx_run.c).
 *
 * The rootfs.cpio is XIP'd from the memory-mapped QUADSPI NOR at 0x90000000.  The coordinator —
 * THIS task: it runs ove_lnx_cpio_to_rootfs + the FDPIC loader — is a PRIVILEGED, non-restricted
 * FreeRTOS-MPU task, so absent an explicit region it reads the NOR through the PRIVDEFENA
 * background map: the 512 MB Normal-cacheable 0x80000000..0x9FFFFFFF block.  With the M7 D-cache
 * on, that view corrupts the reads two ways —
 *   (1) the cache issues 32-byte line-fill BURSTS to the memory-mapped QUADSPI; a non-cacheable
 *       region issues none (proven on silicon: an NC bounded region reads the NOR reliably where
 *       a cacheable / write-through one still faults), and
 *   (2) speculative prefetch within the oversized 512 MB region wanders PAST the 16 MB chip into
 *       unmapped QUADSPI address space.
 * Give THIS task a private MPU region over exactly the mapped NOR — Normal non-cacheable +
 * execute-never: no bursts (1), speculation bounded to the chip (2).  It rides configurable
 * region 0 and, being per-task, leaves the UNPRIVILEGED guest's own cacheable QSPI region
 * (freertos_spawn_common — fast in-place XIP) untouched.  vPortStoreTaskMPUSettings with
 * uxStackDepth==0 preserves this task's stack/all-SRAM region; taskYIELD forces the pended MPU
 * reprogram so the region is live before the very next QUADSPI read (the cpio parse). */
void ove_lnx_rootfs_window(const void *base, size_t len)
{
	MemoryRegion_t regions[portNUM_CONFIGURABLE_REGIONS] = {0};
	regions[0].pvBaseAddress = (void *)(uintptr_t)base;
	regions[0].ulLengthInBytes = (uint32_t)len;
	regions[0].ulParameters = portMPU_REGION_PRIVILEGED_READ_ONLY | portMPU_REGION_EXECUTE_NEVER |
				  (0x08u << portMPU_RASR_TEX_S_C_B_LOCATION); /* TEX=001,S/C/B=0 = Normal NC */
	/* Record it in this task's TCB (configurable region 0) so PendSV re-applies it on every
	 * context switch back to the coordinator — persistent for the whole coordinator life. */
	vTaskAllocateMPURegions(NULL, regions);
	/* vTaskAllocateMPURegions only updates the TCB; the live MPU is not reprogrammed until the
	 * next context switch.  The very next thing the coordinator does is read the NOR (the cpio
	 * parse), which needs the bounded NC view immediately — so program configurable region 0
	 * into the hardware MPU by hand, with the same encoding the port uses on a switch.  Doing it
	 * directly (not via taskYIELD) avoids forcing a context switch from here, which for this task
	 * would be its first switch and trips the FreeRTOS stack-overflow guard. */
	unsigned l2 = 31u - (unsigned)__builtin_clz((unsigned)len); /* log2(len); len is a power of 2 */
	volatile uint32_t *const mpu_rbar = (volatile uint32_t *)0xE000ED9Cu;
	volatile uint32_t *const mpu_rasr = (volatile uint32_t *)0xE000EDA0u;
	*mpu_rbar = (uint32_t)(uintptr_t)base | (1u << 4) /* VALID */ | 0u /* region 0 */;
	*mpu_rasr = 1u /* ENABLE */ | ((l2 - 1u) << 1) /* SIZE field */ | regions[0].ulParameters;
	__asm__ volatile("dsb 0xf\n\tisb 0xf" ::: "memory");
}
#endif

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Strong override of the engine-common weak no-op (backends/common/ove_lnx_run.c).
 *
 * The M7 D-cache runs enabled (write-back) for the personality (drivers/freertos/stm32f7/
 * stm32f7_init.c).  A guest writes its send buffer in external SDRAM through its own Normal WBWA
 * CACHEABLE MPU region (freertos_spawn_common), so just-written bytes can still sit in dirty
 * D-cache lines with stale data in physical SDRAM.  When the guest then calls write()/send(), the
 * PRIVILEGED coordinator runs the lwIP socket copy reading that SAME SDRAM through its uncached
 * (Device) PRIVDEFENA background view — bypassing the cache — and would copy the stale bytes onto
 * the wire.  (Observed as the tail of a mbedTLS ClientHello: the last, most-recently-written
 * extension arrived zeroed, so servers reject the handshake.)  Clean (write back) the buffer's
 * D-cache lines so physical SDRAM — hence the coordinator's uncached view — holds the real bytes.
 * Clean is non-destructive, so an unaligned base/len is safe (CMSIS extends to whole lines). */
void ove_lnx_guest_flush(const void *base, size_t len)
{
	if (len)
		SCB_CleanDCache_by_Addr((uint32_t *)(uintptr_t)base, (int32_t)len);
}
#endif

int ove_lnx_run(const ove_lnx_run_config_t *cfg, const char *path, int argc,
		const char *const argv[])
{
	if (!g_ev) /* create the coordinator wakeup sem in thread context */
		g_ev = xSemaphoreCreateBinaryStatic(&g_ev_buf);
	/* Enable BusFault + UsageFault so a program's bus/usage fault is contained by our handlers
	 * instead of escalating to HardFault (the MPU port's prvSetupMPU only turns on MEMFAULTENA).
	 * SHCSR @ 0xE000ED24: BUSFAULTENA = bit 17, USGFAULTENA = bit 18. */
	*(volatile uint32_t *)0xE000ED24u |= (1u << 17) | (1u << 18);
	return ove_lnx_run_common(&g_freertos_engine, cfg, path, argc, argv);
}
