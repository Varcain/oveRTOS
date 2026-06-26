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

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Real STM32F746 hardware: the MCU has only 320K of internal SRAM — far too small for the 2M
 * region pool + 1M dyn pools — so both live in the board's 8M external SDRAM (0xC0000000) via
 * the linker's .sdram_bss (NOLOAD) section. The board (bsp.c) brings up the FMC controller and
 * makes the SDRAM region executable + Normal non-cacheable (the latter keeps loaded/relocated
 * program code coherent on the M7 with no SCB cache maintenance) before the run loop runs. */
#define OVE_LNX_POOL_SECT __attribute__((section(".sdram_bss"), aligned(32)))
static uint8_t prog_regions[OVE_LNX_NREG][OVE_LNX_PROG_REGION_SIZE] OVE_LNX_POOL_SECT;
static uint8_t dyn_pools[OVE_LNX_NREG][OVE_LNX_DYN_POOL_SIZE] OVE_LNX_POOL_SECT;
#else
static uint8_t prog_regions[OVE_LNX_NREG][OVE_LNX_PROG_REGION_SIZE] __attribute__((aligned(32)));
/* Per-region dynamic-link scratch pool in PSRAM (0x60000000): a dynamic FDPIC proc's arena
 * lives here so ld.so can mmap libc.so (~500K) — far past the in-region 96K arena. an500 RAM
 * (4M) is too tight for this beside the 2M of regions; PSRAM is 16M. NOLOAD → no flash cost. */
static uint8_t dyn_pools[OVE_LNX_NREG][OVE_LNX_DYN_POOL_SIZE]
	__attribute__((section(".psram"), aligned(32)));
#endif
static StaticTask_t g_tcb[OVE_LNX_NSLOT];
static TaskHandle_t g_tid[OVE_LNX_NSLOT];
static StackType_t g_tramp_stacks[OVE_LNX_NSLOT][TRAMP_STACK_WORDS] __attribute__((aligned(8)));

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

/* The C body of the svc trap: build the uniform frame, dispatch, write back. */
void freertos_lnx_svc_c(struct lnx_capture *g)
{
	int sidx = current_slot();
	if (sidx < 0)
		return;
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
}

extern void vPortSVCHandler(void); /* FreeRTOS's own (start-scheduler) handler */

/* SVC vector: while a run is active, capture the frame + dispatch the program's
 * svc; otherwise forward to FreeRTOS (start scheduler). */
__attribute__((naked)) void SVC_Handler(void)
{
	__asm__ volatile("ldr   r1, =g_ove_lnx_active \n"
			 "ldr   r1, [r1]              \n"
			 "cmp   r1, #0                \n"
			 "beq   1f                    \n" /* inactive -> FreeRTOS */
			 "mrs   r0, psp               \n" /* r0 = HW exception frame */
			 "ldr   r1, =g_cap            \n"
			 "str   r0, [r1, #0]          \n" /* g_cap.hw  */
			 "str   r0, [r1, #4]          \n" /* g_cap.psp */
			 "add   r2, r1, #8            \n"
			 "stmia r2, {r4-r11}          \n" /* g_cap.r4_11 */
			 "mov   r0, r1                \n"
			 "push  {lr}                  \n"
			 "bl    freertos_lnx_svc_c    \n"
			 "pop   {lr}                  \n"
			 "bx    lr                    \n" /* exception return: replay frame */
			 "1:                          \n"
			 "b     vPortSVCHandler       \n");
}

/* ---- thread entry trampolines (naked) -------------------------------------- */
struct launch_args { /* arg_tramp reads these by offset — keep the order. */
	void *sp;	      /* +0 */
	void *entry;	      /* +4 */
	void *loadmap;	      /* +8  FDPIC: the exec's elf32_fdpic_loadmap for r7. */
	void *interp_loadmap; /* +12 FDPIC dynamic: ld.so's loadmap for r8 (0 for static). */
	void *got;	      /* +16 FDPIC: the GOT base for r9. ld.so's _start passes the entry
			       *     r9 to _dl_start as its _DYNAMIC ptr, so it MUST be set (the
			       *     program crt overwrites r9, so it's harmless for static). */
};
struct resume_args {
	long r0;
	const struct ove_lnx_resume_ctx *ctx;
};
static struct launch_args g_largs[OVE_LNX_NSLOT];
static struct resume_args g_rargs[OVE_LNX_NSLOT];

__attribute__((naked)) static void arg_tramp(struct launch_args *a __attribute__((unused)))
{
	/* a is in r0. Set the FDPIC entry registers from the struct, switch to the program
	 * stack last (it clobbers sp), then branch. r7 = the exec's loadmap (the crt _start
	 * self-relocates from it); r8 = the interpreter (ld.so) loadmap — 0 for static (the
	 * crt branches on it, so leaving it garbage would fault); r9 = the GOT base — ld.so's
	 * _start passes the ENTRY r9 to _dl_start as its _DYNAMIC pointer, so it MUST be set
	 * for a dynamic exec (the program crt overwrites r9, so 0 is fine for static). Offsets
	 * must match struct launch_args; every program is FDPIC, so r7/r8/r9 are always set. */
	__asm__ volatile("ldr r7, [r0, #8]\n"  /* loadmap */
			 "ldr r8, [r0, #12]\n" /* interp_loadmap */
			 "ldr r9, [r0, #16]\n" /* got */
			 "ldr r1, [r0, #4]\n"  /* entry */
			 "ldr sp, [r0, #0]\n"  /* program sp (last — clobbers sp) */
			 "mov r0, #0\n"
			 "bx  r1\n");
}
static void arg_tramp_task(void *arg)
{
	arg_tramp((struct launch_args *)arg);
}

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
	resume_tramp((void *)a->r0, (void *)a->ctx); /* privileged: read the ctx directly */
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
	g_largs[sidx].sp = sp;
	g_largs[sidx].entry = entry;
	g_largs[sidx].loadmap = prog->is_fdpic ? (void *)prog->loadmap : (void *)0;
	g_largs[sidx].interp_loadmap = prog->is_fdpic ? (void *)prog->interp_loadmap : (void *)0;
	g_largs[sidx].got = prog->is_fdpic ? (void *)prog->got : (void *)0;
	char nm[5] = {'l', 'n', 'x', (char)('0' + sidx), 0}; /* per-slot: ps/top per-proc CPU */
	g_tid[sidx] = xTaskCreateStatic(arg_tramp_task, nm, TRAMP_STACK_WORDS, &g_largs[sidx],
					SLOT_PRIO, g_tramp_stacks[sidx], &g_tcb[sidx]);
	g_ove_lnx_used[sidx] = 1;
	return g_tid[sidx] ? 0 : -1;
}

static void freertos_spawn_resume(int sidx, int ridx, const struct ove_lnx_resume_ctx *ctx,
				  long r0val)
{
	(void)ridx;
	g_rargs[sidx].r0 = r0val;
	g_rargs[sidx].ctx = ctx;
	char nm[5] = {'l', 'n', 'x', (char)('0' + sidx), 0};
	g_tid[sidx] = xTaskCreateStatic(resume_tramp_task, nm, TRAMP_STACK_WORDS, &g_rargs[sidx],
					SLOT_PRIO, g_tramp_stacks[sidx], &g_tcb[sidx]);
	g_ove_lnx_used[sidx] = (g_tid[sidx] != NULL);
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
