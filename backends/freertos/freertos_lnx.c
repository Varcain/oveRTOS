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
	TaskHandle_t t = xTaskGetCurrentTaskHandle();
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

/* Stash the descriptor just below the program SP, INSIDE the program region (xRegions[0]). */
static struct resume_desc *stash_desc(uint32_t sp, const struct ove_lnx_resume_ctx *ctx, long r0)
{
	struct resume_desc *d = (struct resume_desc *)(((sp & ~7u) - sizeof(struct resume_desc)) & ~7u);
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
	/* The program region + dyn_pool live in external SDRAM, which bsp.c maps Normal
	 * NON-cacheable (MPU_TEX_LEVEL1, S/C/B=0). The port default configTEX_S_C_B_SRAM
	 * (0x07 = Normal write-back cacheable + shareable) would re-type our per-task SDRAM
	 * region and precise-BusFault the M7's FMC accesses on real silicon (QEMU doesn't model
	 * the FMC, so the an500 PSRAM is fine with the default). Match the board's SDRAM. */
	const uint32_t tex_s_c_b = 0x08u; /* TEX=001, S=0, C=0, B=0 — Normal non-cacheable */
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
	(void)ridx;
	(void)stack_lo;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	/* The program image was just written into the SDRAM program region through the M7's
	 * D-cache (loader memcpy + relocations). Flush those dirty lines to SDRAM and drop any
	 * stale I-cache so the CPU fetches the real code — without this it executes whatever was
	 * physically in SDRAM (zeroes) and faults. Cheap, once per program launch. */
	SCB_CleanDCache();
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
	(void)freertos_spawn_common(sidx, ridx, stash_desc(ctx->sp, ctx, r0val));
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

int ove_lnx_run(const ove_lnx_run_config_t *cfg, const char *path, int argc,
		const char *const argv[])
{
	if (!g_ev) /* create the coordinator wakeup sem in thread context */
		g_ev = xSemaphoreCreateBinaryStatic(&g_ev_buf);
	return ove_lnx_run_common(&g_freertos_engine, cfg, path, argc, argv);
}
