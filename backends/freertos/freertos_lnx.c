/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * FreeRTOS seam for the Linux personality. The engine-agnostic run loop, svc
 * dispatch, and signal delivery live in modules/lxp/src/lxp_run.c; this file
 * supplies only the FreeRTOS-specific bits: the svc trap, the program memory,
 * and the task spawn (via the lxp_engine vtable).
 *
 * On supported personality boards the program runs as a restricted,
 * UNPRIVILEGED task under the ARM_CM4_MPU port. Its `svc #0` takes the SVCall
 * exception, which this seam OWNS: the board's FreeRTOSConfig.h does NOT alias
 * vPortSVCHandler->SVC_Handler, so the strong SVC_Handler below is the vector; it
 * dispatches the program's svc (while a run is active) to the personality and
 * forwards FreeRTOS's own start-scheduler svc to vPortSVCHandler.
 */

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lxp/lxp_seam.h"
#include "ove/build.h"
#include "ove/time.h"	     /* ove_time_get_us/ns -> engine time_us/time_ns ops */
#include "ove/thread.h"	     /* ove_thread_list -> engine thread_list op */
#include "lxp/lxp_syscall.h"
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
#include "lxp/lxp_netfs.h"
#endif

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#include "bsp.h"       /* bsp_random_fill -> hardware-backed guest entropy */
#include "stm32f7xx.h" /* SCB_CleanDCache / SCB_InvalidateICache: M7 loaded-code coherency */
#endif

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
#include "ove_freertos_lnx_metrics.h"

/* The SVC handler is the only writer. A task-context reader can be preempted by
 * it, but cannot run concurrently on this single-core target. Switching the
 * active window before copying the old one therefore leaves the old window
 * quiescent. The lifetime seqlock protects its 64-bit total from a torn read. */
static volatile uint32_t g_svc_metrics_active;
static struct ove_freertos_lnx_svc_metrics g_svc_metrics_window[2] = {
	{.min_cycles = UINT32_MAX},
	{.min_cycles = UINT32_MAX},
};
static volatile uint32_t g_svc_metrics_total_seq;
static struct ove_freertos_lnx_svc_metrics g_svc_metrics_total = {
	.min_cycles = UINT32_MAX,
};

static void svc_metrics_add(struct ove_freertos_lnx_svc_metrics *metrics, uint32_t syscall,
			    uint32_t cycles)
{
	if (metrics->calls == 0u || cycles < metrics->min_cycles)
		metrics->min_cycles = cycles;
	if (metrics->calls == 0u || cycles > metrics->max_cycles) {
		metrics->max_cycles = cycles;
		metrics->max_syscall = syscall;
	}
	metrics->calls++;
	metrics->total_cycles += cycles;
}

static void svc_metrics_record(uint32_t syscall, uint32_t cycles)
{
	uint32_t active = g_svc_metrics_active;
	svc_metrics_add(&g_svc_metrics_window[active], syscall, cycles);

	g_svc_metrics_total_seq++;
	__asm__ volatile("" ::: "memory");
	svc_metrics_add(&g_svc_metrics_total, syscall, cycles);
	__asm__ volatile("" ::: "memory");
	g_svc_metrics_total_seq++;
}

void ove_freertos_lnx_svc_metrics_take(struct ove_freertos_lnx_svc_metrics *window,
				       struct ove_freertos_lnx_svc_metrics *total)
{
	uint32_t old_active = g_svc_metrics_active;
	g_svc_metrics_active = old_active ^ 1u;
	__asm__ volatile("" ::: "memory");

	*window = g_svc_metrics_window[old_active];
	g_svc_metrics_window[old_active] = (struct ove_freertos_lnx_svc_metrics){
		.min_cycles = UINT32_MAX,
	};

	uint32_t before;
	uint32_t after;
	do {
		before = g_svc_metrics_total_seq;
		__asm__ volatile("" ::: "memory");
		total->calls = g_svc_metrics_total.calls;
		total->min_cycles = g_svc_metrics_total.min_cycles;
		total->max_cycles = g_svc_metrics_total.max_cycles;
		total->total_cycles = g_svc_metrics_total.total_cycles;
		total->max_syscall = g_svc_metrics_total.max_syscall;
		__asm__ volatile("" ::: "memory");
		after = g_svc_metrics_total_seq;
	} while (before != after || (after & 1u) != 0u);
}

uint32_t ove_freertos_lnx_svc_counter_hz(void)
{
	return SystemCoreClock;
}
#endif /* CONFIG_OVE_LINUX_RT_SCOPE */

#define TRAMP_STACK_WORDS 192u		  /* tramp prologue; the program uses its own stack */
#define TRAMP_STORAGE_WORDS 256u	  /* 768-byte stack + 256-byte resume handoff */
#define SLOT_PRIO (tskIDLE_PRIORITY + 1u) /* below the run-loop task (its creator) */
#ifndef CONFIG_OVE_LINUX_GUEST_QUANTUM_MS
#define CONFIG_OVE_LINUX_GUEST_QUANTUM_MS 10
#endif

/* Under the ARM_CM4_MPU port the task's privilege rides in the top bit of its priority
 * (portPRIVILEGE_BIT). The restricted-task path below deliberately omits that bit. The
 * non-MPU fallback defines it as zero only so legacy non-personality builds still compile. */
#ifndef portPRIVILEGE_BIT
#define portPRIVILEGE_BIT 0u
#endif
#define SLOT_PROG_PRIO (SLOT_PRIO | portPRIVILEGE_BIT)

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Real STM32F746 hardware: the MCU has only 320K of internal SRAM — far too small for the
 * program/dynamic pools — so they live in the board's 8M external SDRAM (0xC0000000) via
 * the linker's .sdram_bss (NOLOAD) section. The board (bsp.c) brings up the FMC controller and
 * installs a temporary Normal non-cacheable view for pre-scheduler SDRAM access; the MPU port
 * replaces it at scheduler start, and the seam gives each guest cacheable per-task overlays. */
#define LXP_EXT_STORAGE_SECTION ".sdram_bss.lxp"
#else
/* Both pools live in PSRAM (0x60000000, 16M; NOLOAD → no flash cost). MPU isolation requires
 * the program's per-task regions over its program region + dyn_pool,
 * and PMSAv7 requires each region's base to be aligned to its (power-of-2) size — so both arrays
 * are size-aligned. PSRAM also keeps them off the kernel's 4M SRAM (the dynamic FDPIC proc's
 * arena anyway needs room to mmap libc.so ~500K, far past the in-region 96K arena). */
#define LXP_EXT_STORAGE_SECTION ".psram.lxp"
#endif
/* Keep the largest-alignment rows first, then the smaller program rows and cold
 * exec captures. One explicitly ordered object avoids alignment holes and makes
 * odd LXP_NREG values safe on PMSAv7. The linker places this object before other
 * external-BSS consumers. */
struct lxp_ext_storage {
	uint8_t dyn_pools[LXP_NREG][LXP_DYN_POOL_SIZE];
	uint8_t prog_regions[LXP_NREG][LXP_PROG_REGION_SIZE];
	lxp_exec_capture_t exec_captures[LXP_NSLOT];
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	uint8_t netfs_exec_stage[256u * 1024u];
#endif
};
static struct lxp_ext_storage g_lxp_ext_storage
	__attribute__((section(LXP_EXT_STORAGE_SECTION), aligned(LXP_DYN_POOL_SIZE)));
_Static_assert(offsetof(struct lxp_ext_storage, prog_regions) % LXP_PROG_REGION_SIZE == 0,
	       "program rows must be aligned to their MPU region size");
#define dyn_pools (g_lxp_ext_storage.dyn_pools)
#define prog_regions (g_lxp_ext_storage.prog_regions)
#define g_exec_captures (g_lxp_ext_storage.exec_captures)
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
#define g_netfs_exec_stage (g_lxp_ext_storage.netfs_exec_stage)
#endif
static StaticTask_t g_tcb[LXP_NSLOT];
static TaskHandle_t g_tid[LXP_NSLOT];
static uint32_t g_task_generation[LXP_NSLOT];
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
/* A remote-exec proc runs its OWN text from RAM in its program region, so that region is mapped
 * RWX (a per-process, MPU-contained W^X relaxation). Set at launch; kept across resumes because
 * freertos_spawn_common re-applies the MPU on every resume. */
static uint8_t g_region_exec[LXP_NSLOT];
/* Staging buffer for a fetched remote ELF: the netfs layer fills it, the loader copies its text
 * into the program region. In SDRAM (STM32) / PSRAM (an500), NOLOAD → no flash cost. Sized for a
 * small/medium dynamic FDPIC binary (its own text+data; libc/ld.so stay XIP from the local rootfs). */
static uint8_t *freertos_exec_stage(size_t *cap)
{
	if (cap)
		*cap = sizeof(g_netfs_exec_stage);
	return g_netfs_exec_stage;
}
#endif
/* Each allocation is a 1K-aligned PMSAv7 stack region. The task uses the
 * bottom 768 bytes; the top 256 bytes retain its persistent resume handoff. */
static StackType_t g_tramp_stacks[LXP_NSLOT][TRAMP_STORAGE_WORDS]
	__attribute__((aligned(TRAMP_STORAGE_WORDS * sizeof(StackType_t))));

/* Exception-containment code must never issue a VFP instruction.  A guest can
 * fault while lazy FP preservation is pending and its PSP is already invalid;
 * touching the FPU in that state retries the failed lazy store and escalates
 * the configurable fault to HardFault before the guest can be reaped. */
#define LXP_FAULT_GPR_ONLY __attribute__((target("general-regs-only")))

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
	for (int i = 0; i < LXP_NSLOT; i++)
		if (g_lxp_used[i] && g_tid[i] == t)
			return i;
	return -1;
}

/* Guest-only round robin for builds which deliberately leave FreeRTOS's global
 * 1 ms equal-priority slicing disabled. SysTick calls this after the scheduler
 * tick. Once one guest consumes its configured budget while a peer is ready,
 * PendSV rotates the ready list at SLOT_PRIO. Higher-priority host work is
 * unaffected and may preempt at any point in the budget. */
void ove_freertos_lxp_tick(void)
{
#if (configUSE_TIME_SLICING == 0)
	static uint32_t budget_ticks;
	static TaskHandle_t budget_owner;
	int current = current_slot();
	if (!g_lxp_active || current < 0) {
		budget_ticks = 0;
		budget_owner = NULL;
		return;
	}
	if (budget_owner != g_tid[current]) {
		budget_owner = g_tid[current];
		budget_ticks = 0;
	}
	uint32_t quantum_ticks =
		((uint32_t)CONFIG_OVE_LINUX_GUEST_QUANTUM_MS *
			 (uint32_t)configTICK_RATE_HZ +
		 999u) /
		1000u;
	if (quantum_ticks == 0)
		quantum_ticks = 1;
	if (++budget_ticks < quantum_ticks)
		return;
	budget_ticks = 0;
	for (int s = 0; s < LXP_NSLOT; s++) {
		if (s != current && g_lxp_used[s] && g_tid[s]) {
			portYIELD_FROM_ISR(pdTRUE);
			return;
		}
	}
#endif
}

/* ---- the SVC trap ---------------------------------------------------------- */
/* The HW-stacked exception frame (live on the program PSP) + the callee-saved
 * registers the asm shim captures before they are clobbered. */
struct lnx_capture {
	uint32_t *hw;	   /* hw[0..7] = r0,r1,r2,r3,r12,lr,pc,xpsr */
	uint32_t psp;	   /* the program SP at the HW frame */
	uint32_t r4_11[8]; /* r4..r11 */
	uint32_t exc_return;
#if LXP_ENABLE_FPU_CONTEXT
	struct lxp_fp_context fp;
#endif
};
/* SVC_Handler stores into g_cap by hardcoded byte offset (str r0,[r1,#0]; #4; add r2,r1,#8;
 * str lr,[r1,#40]) — pin each so a struct-layout change is a build error, not silent corruption
 * of the syscall-capture path. */
_Static_assert(offsetof(struct lnx_capture, hw) == 0u, "SVC capture hw offset");
_Static_assert(offsetof(struct lnx_capture, psp) == 4u, "SVC capture psp offset");
_Static_assert(offsetof(struct lnx_capture, r4_11) == 8u, "SVC capture r4-r11 offset");
_Static_assert(offsetof(struct lnx_capture, exc_return) == 40u, "SVC capture EXC_RETURN offset");
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
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	uint32_t svc_start_cycles = DWT->CYCCNT;
#endif
	int sidx = current_slot();
	if (sidx < 0)
		return 0; /* not a program task → forward to FreeRTOS */
	struct lxp_frame f;
	memset(&f, 0, sizeof(f));
	uint32_t fp_frame_bytes = 0;
#if LXP_ENABLE_FPU_CONTEXT
	f.fp = &g->fp;
	memset(&g->fp, 0, sizeof(g->fp));
	if ((g->exc_return & (1u << 4)) == 0) {
		/* With lazy preservation enabled, the extended frame can be reserved but
		 * its s0-s15 contents are not valid until the first handler-mode VFP
		 * instruction. Preserve s0 on MSP while forcing that pending operation. */
		__asm__ volatile("vpush {s0}\n"
				 "vpop  {s0}\n"
				 :
				 :
				 : "memory");
		for (int i = 0; i < 16; i++)
			g->fp.s[i] = g->hw[8 + i];
		uint32_t *high = &g->fp.s[16];
		__asm__ volatile("vstmia %0, {s16-s31}" : : "r"(high) : "memory");
		g->fp.fpscr = g->hw[24];
		g->fp.active = 1;
		fp_frame_bytes = 18u * sizeof(uint32_t);
	}
#endif
	f.r[0] = g->hw[0];
	f.r[1] = g->hw[1];
	f.r[2] = g->hw[2];
	f.r[3] = g->hw[3];
	for (int i = 0; i < 8; i++)
		f.r[4 + i] = g->r4_11[i];
	f.r[12] = g->hw[4];
	/* The guest ABI observes the SP before exception entry. An extended FP frame
	 * adds s0-s15, FPSCR and one reserved word after the 8-word core frame. */
	f.r[13] = g->psp + 32u + fp_frame_bytes + ((g->hw[7] & (1u << 9)) ? 4u : 0u);
	f.r[14] = g->hw[5];
	f.r[15] = g->hw[6];
	f.xpsr = g->hw[7];

	lxp_dispatch(&f, &g_lxp_proc[sidx]);

	g->hw[0] = f.r[0];
	g->hw[1] = f.r[1];
	g->hw[2] = f.r[2];
	g->hw[3] = f.r[3];
	g->hw[4] = f.r[12];
	g->hw[5] = f.r[14];
	g->hw[6] = f.r[15];
	g->hw[7] = f.xpsr;
	/* Write r4-r11 back so a dispatch that rewrites a callee-saved register on the fast path takes
	 * effect. rt_sigreturn restores the interrupted code's r9 (FDPIC GOT) via sig_restore — a signal
	 * handler runs with its OWN r9, so without this the interrupted syscall resumes with the handler's
	 * GOT and its __errno_location PLT resolves through the wrong module (-> sigaction -> SIGSEGV).
	 * For every other syscall these equal the captured values, so SVC_Handler's reload is a no-op. */
	for (int i = 0; i < 8; i++)
		g->r4_11[i] = f.r[4 + i];
#if LXP_ENABLE_FPU_CONTEXT
	if ((g->exc_return & (1u << 4)) == 0) {
		for (int i = 0; i < 16; i++)
			g->hw[8 + i] = g->fp.s[i];
		g->hw[24] = g->fp.fpscr;
		uint32_t *high = &g->fp.s[16];
		__asm__ volatile("vldmia %0, {s16-s31}" : : "r"(high) : "memory");
	}
#endif
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	/* Read the counter before updating the statistics so the observer's own
	 * bookkeeping is not charged to the syscall it records. Unsigned
	 * subtraction is safe across CYCCNT's approximately 19.9-second wrap. */
	svc_metrics_record(f.r[7], DWT->CYCCNT - svc_start_cycles);
#endif
	return 1;
}

extern void vPortSVCHandler(void); /* FreeRTOS's own (start-scheduler / yield / priv) handler */

/* SVC vector: while a run is active, capture the frame + dispatch the program's
 * svc; otherwise forward to FreeRTOS (start scheduler). */
__attribute__((naked)) void SVC_Handler(void)
{
	__asm__ volatile(
		"ldr   r1, =g_lxp_active \n"
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
		"str   lr, [r1, #40]         \n" /* EXC_RETURN selects basic/extended frame */
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
		/* Reload r4-r11 from g_cap (freertos_lnx_svc_c wrote them back post-dispatch): the
			  * exception return only replays the HW frame (r0-r3,r12,lr,pc,xpsr), so a callee-saved
			  * register the dispatch rewrote (rt_sigreturn's r9/FDPIC-GOT restore) would otherwise be
			  * dropped and the interrupted code resumes with the signal handler's GOT. */
		"ldr   r1, =g_cap            \n"
		"add   r1, r1, #8            \n"
		"ldmia r1, {r4-r11}          \n"
		"bx    lr                    \n" /* 1 = handled: exception return, replay frame */
		"1:                          \n"
		"b     vPortSVCHandler       \n");
}

/* ---- MemManage fault containment ------------------------------------------- */
/* An UNPRIVILEGED program making an illegal access raises MemManage (enabled by the MPU port's
 * prvSetupMPU). Contain it like a default-action SIGSEGV: publish a typed exit intent (139),
 * synthesize a trusted return frame on its internal trampoline stack, wake the coordinator (which
 * reaps the slot + frees the region via EV_EXIT), and exception-return to the park loop. A fault
 * from any NON-program (privileged kernel) context is a real bug -> fatal. Strong symbol overriding
 * the weak MemManage_Handler in the CMSIS startup. */
static void freertos_event_post(void) LXP_FAULT_GPR_ONLY; /* defined with the vtable below */

/* Terminal handler for a fault that cannot be attributed to a guest — i.e. a fault in host /
 * privileged context (the coordinator, an ISR, or before any guest is live). Such a fault means the
 * trusted side is compromised, so the only safe action is to STOP; recovering as if it were a guest
 * would run the host on corrupted state. Weak default: halt (a watchdog, if armed, reboots). The
 * board overrides it to print a diagnostic first. Runs in fault context, so it must never return
 * and must not touch VFP (general-regs-only) — an in-flight lazy-FP stack to an invalid frame would
 * nest another fault. */
__attribute__((weak)) LXP_FAULT_GPR_ONLY void ove_lnx_host_fatal(uint32_t cfsr, uint32_t hfsr,
								 uint32_t pc)
{
	(void)cfsr;
	(void)hfsr;
	(void)pc;
	for (;;) {
	}
}

struct lnx_fault_diag {
	uint32_t count;
	uint32_t cfsr;
	uint32_t hfsr;
	uint32_t mmfar;
	uint32_t bfar;
	uint32_t psp;
	uint32_t exc_return;
	uint32_t last_spawn_sp;
	uint32_t last_spawn_pc;
	uint32_t last_desc;
	uint32_t last_ridx;
	uint32_t last_kind; /* 1 = image launch, 2 = context resume */
};
/* Kept in host SRAM and intentionally non-static so a stopped target can be
 * diagnosed without logging or formatting in fault context. */
volatile struct lnx_fault_diag g_lxp_fault_diag[LXP_NSLOT];

uint32_t *LXP_FAULT_GPR_ONLY freertos_lnx_memfault_c(uint32_t exc_return, uint32_t psp)
{
	int sidx = current_slot();
	/* A Linux guest always runs in Thread mode on PSP.  Do not misclassify a
	 * nested host/ISR fault merely because a guest TCB is current. */
	if (g_lxp_active && sidx >= 0 && (exc_return & (1u << 3)) && (exc_return & (1u << 2))) {
		volatile struct lnx_fault_diag *diag = &g_lxp_fault_diag[sidx];
		diag->count++;
		diag->cfsr = *(volatile uint32_t *)0xE000ED28u;
		diag->hfsr = *(volatile uint32_t *)0xE000ED2Cu;
		diag->mmfar = *(volatile uint32_t *)0xE000ED34u;
		diag->bfar = *(volatile uint32_t *)0xE000ED38u;
		diag->psp = psp;
		diag->exc_return = exc_return;

		g_lxp_proc[sidx].exit_status = 139; /* 128 + SIGSEGV */
		g_lxp_proc[sidx].exit_reason = LXP_EXIT_REASON_MEMORY_FAULT;
		g_lxp_proc[sidx].exit_signal = LXP_SIGSEGV;
		g_lxp_proc[sidx].exit_detail = diag->cfsr;
		g_lxp_proc[sidx].exit_address = (diag->cfsr & (1u << 7))    ? diag->mmfar
						: (diag->cfsr & (1u << 15)) ? diag->bfar
									    : 0u;
		(void)lxp_intent_exit(&g_lxp_proc[sidx], 0);

		/* Never trust the faulting PSP frame: MSTKERR/MLSPERR means it may not
		 * exist at all, and an arbitrary guest PSP may point at read-only QSPI or
		 * host memory.  Build a basic frame at the top of the task's original
		 * internal-SRAM trampoline stack, which is still its automatic MPU stack
		 * region.  Exception return consumes the frame and enters the stackless
		 * park loop; the already-pended coordinator then deletes the task. */
		volatile uint32_t *frame =
			(uint32_t *)&g_tramp_stacks[sidx][TRAMP_STACK_WORDS - 8u];
		frame[0] = 0;
		frame[1] = 0;
		frame[2] = 0;
		frame[3] = 0;
		frame[4] = 0;
		frame[5] = 0;
		frame[6] = ((uint32_t)&lxp_park_loop) & ~1u;
		frame[7] = (1u << 24); /* xPSR.T (Thumb) */

		*(volatile uint32_t *)0xE000ED28u = *(
			volatile uint32_t *)0xE000ED28u; /* clear configurable status bits (W1C) */
		lxp_event_post_slot(sidx);
		return (uint32_t *)frame;
	}
	/* Not a guest fault: host/privileged context, handler mode, or no active guest. Capture the
	 * fault registers and the faulting PC (offset 6 of a Thread-mode PSP frame), then go fatal —
	 * ove_lnx_host_fatal reports and halts, and does not return. */
	uint32_t cfsr = *(volatile uint32_t *)0xE000ED28u;
	uint32_t hfsr = *(volatile uint32_t *)0xE000ED2Cu;
	uint32_t pc = ((exc_return & (1u << 2)) && psp) ? ((volatile uint32_t *)psp)[6] : 0u;
	ove_lnx_host_fatal(cfsr, hfsr, pc);
	for (;;) { /* belt-and-suspenders: the override must not return */
	}
}

__attribute__((naked)) void MemManage_Handler(void)
{
	__asm__ volatile("mrs  r1, psp                \n" /* r1 = untrusted fault-time PSP */
			 /* Same nPRIV dance as SVC_Handler: memfault_c calls FreeRTOS APIs
			  * (event_post) which must not raise-privilege via svc from here. */
			 "mrs  r2, control            \n"
			 "push {r2, lr}               \n"
			 "bic  r3, r2, #1             \n"
			 "msr  control, r3            \n"
			 "isb                         \n"
			 /* Cancel a pending lazy FP store before entering compiled code.  If
			  * the guest PSP caused MLSPERR, any VFP use before this write would
			  * immediately refault.  FPCCR.LSPACT is architecturally R/W. */
			 "ldr  r3, =0xe000ef34        \n"
			 "ldr  r0, [r3]               \n"
			 "bic  r0, r0, #1             \n"
			 "str  r0, [r3]               \n"
			 "dsb                         \n"
			 "isb                         \n"
			 "mov  r0, lr                 \n" /* r0 = original EXC_RETURN */
			 "bl   freertos_lnx_memfault_c\n"
			 "cbz  r0, 1f                 \n"
			 "msr  psp, r0                \n" /* trusted internal-SRAM basic frame */
			 "pop  {r2, lr}               \n"
			 "bic  r2, r2, #4             \n" /* CONTROL.FPCA = 0 */
			 "msr  control, r2            \n"
			 "orr  lr, lr, #0x10          \n" /* exception return uses basic frame */
			 "isb                         \n"
			 "bx   lr                     \n"
			 "1:                          \n"
			 "pop  {r2, lr}               \n"
			 "b    HardFault_Handler       \n");
}

/* UsageFault (undefined instruction / bad control flow) + BusFault get the SAME containment as
 * MemManage: a program fault is killed (139), a kernel fault is fatal. Both tail-branch into
 * MemManage_Handler's body (which reads the faulting PSP frame + calls freertos_lnx_memfault_c —
 * fault-type-agnostic, and its CFSR write-back already clears BFSR/UFSR too). Enabled via SHCSR in
 * lxp_run; the MPU port's prvSetupMPU only turns on MEMFAULTENA. */
__attribute__((naked)) void UsageFault_Handler(void)
{
	__asm__ volatile("b MemManage_Handler");
}
__attribute__((naked)) void BusFault_Handler(void)
{
	__asm__ volatile("b MemManage_Handler");
}

/* ---- thread entry: task-local descriptor + unified trampoline -------------- */
/* The program's entry/resume context is stashed in the user-readable tail of
 * its bootstrap stack, so the unprivileged trampoline can consume it while the
 * privileged coordinator can update it directly. */
struct resume_desc {
	uint32_t r0;
	struct lxp_resume_ctx ctx;
	volatile uint32_t ready;
};
static struct resume_desc *g_park_desc[LXP_NSLOT];

/* prog_tramp is naked asm that reaches ctx as r0+4 and then loads every core register by hardcoded
 * offset (ldmia for r4-r11, then r12/lr/sp/pc, then r1/r2/r3/xpsr). Pin each so a change to
 * lxp_resume_ctx (an LXP header) breaks the build instead of silently resuming a guest onto the
 * wrong registers. Layout-stable regardless of the appended FP block, so unconditional. */
_Static_assert(offsetof(struct resume_desc, ctx) == 4u, "resume ctx offset (add r3,r0,#4)");
_Static_assert(offsetof(struct resume_desc, ctx.r4_11) == 4u, "resume r4-r11 offset");
_Static_assert(offsetof(struct resume_desc, ctx.r12) == 36u, "resume r12 offset");
_Static_assert(offsetof(struct resume_desc, ctx.lr) == 40u, "resume lr offset");
_Static_assert(offsetof(struct resume_desc, ctx.sp) == 44u, "resume sp offset");
_Static_assert(offsetof(struct resume_desc, ctx.pc) == 48u, "resume pc offset");
_Static_assert(offsetof(struct resume_desc, ctx.r1) == 52u, "resume r1 offset");
_Static_assert(offsetof(struct resume_desc, ctx.r2) == 56u, "resume r2 offset");
_Static_assert(offsetof(struct resume_desc, ctx.r3) == 60u, "resume r3 offset");
_Static_assert(offsetof(struct resume_desc, ctx.xpsr) == 64u, "resume xpsr offset");

#if LXP_ENABLE_FPU_CONTEXT
/* prog_tramp is naked assembly, so pin every optional field offset it consumes.
 * Appending FP state leaves all established core-register offsets unchanged. */
_Static_assert(offsetof(struct resume_desc, ctx.fp.s) == 68u, "resume FP register offset");
_Static_assert(offsetof(struct resume_desc, ctx.fp.fpscr) == 196u, "resume FPSCR offset");
_Static_assert(offsetof(struct resume_desc, ctx.fp.active) == 200u, "resume FP-active offset");
#define LXP_TRAMP_RESTORE_FP                           \
	"ldr   r1, [r0, #200]  \n" /* ctx.fp.active */ \
	"cbz   r1, 0f          \n"                     \
	"add   r2, r0, #68     \n" /* ctx.fp.s */      \
	"vldmia r2!, {s0-s31}  \n"                     \
	"ldr   r1, [r0, #196]  \n" /* ctx.fp.fpscr */  \
	"vmsr  fpscr, r1       \n"                     \
	"0:                    \n"
#else
#define LXP_TRAMP_RESTORE_FP ""
#endif

__attribute__((naked)) static void prog_tramp(void *desc __attribute__((unused)))
{
	/* Restore the complete syscall-visible context. APSR.NZCVQ matters: an immediate
	 * hardware exception return preserves flags, and optimized userspace may carry a
	 * comparison across its next syscall. Stage PC below the guest SP so r1-r3 can all
	 * reach their final values before the branch. */
	__asm__ volatile(LXP_TRAMP_RESTORE_FP
			 "add   r3, r0, #4     \n" /* r3 -> ctx */
			 "ldmia r3!, {r4-r11} \n"
			 "ldr   r12, [r3], #4 \n"
			 "ldr   lr,  [r3], #4 \n"
			 "ldr   r1,  [r3], #4 \n" /* ctx.sp (temp) */
			 "ldr   r2,  [r3], #4 \n" /* ctx.pc (temp); r3 -> ctx.r1 */
			 "mov   sp,  r1       \n"
			 "ldr   r1,  [r3, #12]\n" /* ctx.xpsr */
			 "msr   APSR_nzcvq, r1\n"
			 "push  {r2}          \n" /* stage ctx.pc */
			 "ldr   r1,  [r3]     \n"
			 "ldr   r2,  [r3, #4] \n"
			 "ldr   r0,  [r0]     \n"
			 "ldr   r3,  [r3, #8] \n"
			 "pop   {pc}           \n");
}

/* Bytes the descriptor occupies, rounded up to the MPU/cache-line granularity. */
#define RESUME_DESC_CLSPAN (((uint32_t)sizeof(struct resume_desc) + 31u) & ~31u)
_Static_assert(RESUME_DESC_CLSPAN <=
		       (TRAMP_STORAGE_WORDS - TRAMP_STACK_WORDS) * sizeof(StackType_t),
	       "resume descriptor exceeds reserved trampoline-stack tail");

/* Reserve the top 256 bytes of each aligned 1K bootstrap-stack allocation for
 * the persistent resume descriptor. The FreeRTOS task receives the lower 768
 * bytes as its logical stack; the ARM MPU port rounds that 768-byte stack region
 * to the containing aligned 1K region, so the unprivileged park loop can read
 * the tail without consuming another configurable MPU region. Unlike storage
 * below ctx->sp, this address is independent of Linux stack depth and cannot be
 * overwritten by exception or context-switch frames. */
static struct resume_desc *stash_desc(int sidx, const struct lxp_resume_ctx *ctx, long r0)
{
	struct resume_desc *d = (struct resume_desc *)&g_tramp_stacks[sidx][TRAMP_STACK_WORDS];
	d->r0 = (uint32_t)r0;
	d->ctx = *ctx;
	d->ready = 1u;
	return d;
}

/* Exception-side half of the persistent handoff. The descriptor remains in the
 * task's user-readable bootstrap-stack MPU region while it is suspended. */
static void *freertos_park_prepare(int sidx, uint32_t generation,
				   const struct lxp_resume_ctx *ctx)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || !g_tid[sidx] ||
	    g_task_generation[sidx] != generation)
		return NULL;
	struct resume_desc *d = stash_desc(sidx, ctx, 0);
	__atomic_store_n(&d->ready, 0u, __ATOMIC_RELEASE);
	g_park_desc[sidx] = d;
	return d;
}

/* Runs unprivileged in the existing guest task. Normally the higher-priority
 * coordinator suspends the task before exception return; the bounded race is a
 * read-only wait on guest memory. spawn_resume publishes the descriptor and
 * resumes this same task, which restores the complete Linux register context. */
void lxp_park_loop(void *token)
{
	struct resume_desc *d = token;
	while (!__atomic_load_n(&d->ready, __ATOMIC_ACQUIRE))
		__asm__ volatile("nop");
	prog_tramp(d);
	__builtin_unreachable();
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

/* Spawn the program task entering prog_tramp. Supported personality boards use the MPU branch: a
 * RESTRICTED, UNPRIVILEGED task whose only RW regions are its program region + dyn_pool (both XN;
 * ordinary code XIPs from a separate RO+X rootfs window). The legacy non-MPU compile fallback
 * creates a plain privileged task and is not used by the STM32F746 or an500 personality targets. */
static int freertos_spawn_common(int sidx, int ridx, struct resume_desc *desc)
{
	char nm[6];
	slot_task_name(nm, sidx); /* diagnostic only; attribution uses the task handle */
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
	const uint32_t tex_s_c_b =
		0x0Bu; /* TEX=001, S=0, C=1, B=1 — Normal WBWA cacheable, non-shareable */
#else
	const uint32_t tex_s_c_b = configTEX_S_C_B_SRAM; /* an500 PSRAM: port default 0x07 */
#endif
	const uint32_t rw_xn = portMPU_REGION_READ_WRITE | portMPU_REGION_EXECUTE_NEVER |
			       (tex_s_c_b << portMPU_RASR_TEX_S_C_B_LOCATION);
	uint32_t reg0_attr = rw_xn; /* program region: RW + execute-never (code XIPs from rootfs) */
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	/* A remote-exec proc runs its own copied text FROM this region → map it RWX (drop XN). A
	 * per-process, MPU-contained W^X relaxation, only for a program launched off the remote mount. */
	if (g_region_exec[sidx])
		reg0_attr = portMPU_REGION_READ_WRITE |
			    (tex_s_c_b << portMPU_RASR_TEX_S_C_B_LOCATION);
#endif
	/* pxTaskBuffer is `StaticTask_t * const`, so a designated initializer is required (it also
	 * zeroes the remaining configurable region xRegions[2]). xRegions[0] = the program region,
	 * xRegions[1] = the dyn_pool — both RW + execute-never (code runs from the RO+X rootfs). */
	TaskParameters_t tp = {
		.pvTaskCode = prog_tramp,
		.pcName = nm,
		.usStackDepth = TRAMP_STACK_WORDS,
		.pvParameters = desc,
		.uxPriority = SLOT_PRIO, /* NO portPRIVILEGE_BIT -> UNPRIVILEGED */
		.puxStackBuffer = g_tramp_stacks[sidx],
		.pxTaskBuffer = &g_tcb[sidx],
		.xRegions =
			{
				{prog_regions[ridx], LXP_PROG_REGION_SIZE, reg0_attr},
				{dyn_pools[ridx], LXP_DYN_POOL_SIZE, rw_xn},
			},
	};
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	/* The guest XIPs its FDPIC text in-place from the rootfs cpio in the on-board QSPI NOR
	 * (memory-mapped at 0x90000000). The static unprivileged-flash MPU region stays on internal
	 * flash — that is where prog_tramp / lxp_park_loop / the resume stubs live, which this
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
	tp.xRegions[2].ulParameters = portMPU_REGION_READ_ONLY |
				      (0x02u << portMPU_RASR_TEX_S_C_B_LOCATION);
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
	/* The guest XIPs its FDPIC text in-place from the rootfs cpio in PSRAM — QEMU loads it at
	 * 0x60000000 via `-device loader` (see qemu-run.sh; the QEMU analog of the STM32 QSPI XIP).
	 * prog_tramp / lxp_park_loop still execute from internal flash (the static unprivileged-RX
	 * flash region), so the PSRAM cpio needs its own per-task RX window. The cpio occupies the
	 * bottom 12 MiB of PSRAM (linker .psram_rootfs), the program pools the top 4 MiB. Cover
	 * exactly that 12 MiB cpio window with two power-of-2 regions (8 MiB + 4 MiB) so it does NOT
	 * overlap the pools' RW regions — a single 16 MiB region would, and being higher-priority
	 * would force the guest's own dyn_pool read-only. Unprivileged RO + executable (W^X: the
	 * guest never writes the cpio). xRegions[2],[3] — an500 has 11 configurable MPU regions. */
	tp.xRegions[2].pvBaseAddress = (void *)0x60000000u;
	tp.xRegions[2].ulLengthInBytes = 8u * 1024u * 1024u;
	tp.xRegions[2].ulParameters = portMPU_REGION_READ_ONLY |
				      (configTEX_S_C_B_SRAM << portMPU_RASR_TEX_S_C_B_LOCATION);
	tp.xRegions[3].pvBaseAddress = (void *)0x60800000u;
	tp.xRegions[3].ulLengthInBytes = 4u * 1024u * 1024u;
	tp.xRegions[3].ulParameters = portMPU_REGION_READ_ONLY |
				      (configTEX_S_C_B_SRAM << portMPU_RASR_TEX_S_C_B_LOCATION);
#endif
	BaseType_t ok = xTaskCreateRestrictedStatic(&tp, &g_tid[sidx]);
	return (ok == pdPASS) ? 0 : -1;
#else
	(void)ridx;
	g_tid[sidx] = xTaskCreateStatic(prog_tramp, nm, TRAMP_STACK_WORDS, desc, SLOT_PROG_PRIO,
					g_tramp_stacks[sidx], &g_tcb[sidx]);
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
		*size = LXP_DYN_POOL_SIZE;
	return dyn_pools[ridx];
}

static lxp_exec_capture_t *freertos_exec_capture(int sidx)
{
	return (sidx >= 0 && sidx < LXP_NSLOT) ? &g_exec_captures[sidx] : NULL;
}

static int freertos_spawn_launch(int sidx, uint32_t generation, int ridx,
				 const lxp_flat_t *prog, void *entry, void *sp,
				 void *stack_lo)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || generation == 0 || g_tid[sidx])
		return -1;
	(void)stack_lo;
	g_park_desc[sidx] = NULL;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	/* The loader wrote this program's image (data + relocations) to the SDRAM program region
	 * through the coordinator's uncached (Device background) view, and materialised new code paths.
	 * With the D-cache ON the region is Normal WBWA cacheable (freertos_spawn_common), so INVALIDATE
	 * exactly this region before the guest runs: drop stale cacheable lines from the previous tenant
	 * of this ridx so the guest's first reads miss + fill the fresh image from SDRAM.  Invalidate —
	 * NOT clean — a clean would write those stale lines back OVER the loader's fresh SDRAM.  (D-cache
	 * off: no lines to drop; skip the walk.)  Then invalidate the I-cache so the CPU fetches the real
	 * code rather than whatever was physically in SDRAM. */
	if (SCB->CCR & SCB_CCR_DC_Msk) {
		/* freertos_coord_map now gives the loader a CACHEABLE view of this region, so its
		 * image writes can sit in dirty D-cache lines. CLEAN (write back to SDRAM so the
		 * I-cache refills the real code) then INVALIDATE (drop the previous tenant's stale
		 * lines) both the program region and the dyn_pool — a dynamic exec's ld.so lays
		 * libc text into the latter. If the D-cache view was uncached (initial launch,
		 * before any coord_map) the clean is a harmless no-op and this reduces to the
		 * previous invalidate. */
		SCB_CleanInvalidateDCache_by_Addr((void *)prog_regions[ridx],
						  (int32_t)LXP_PROG_REGION_SIZE);
		SCB_CleanInvalidateDCache_by_Addr((void *)dyn_pools[ridx],
						  (int32_t)LXP_DYN_POOL_SIZE);
	}
	SCB_InvalidateICache();
	__DSB();
	__ISB();
#endif
	/* FDPIC entry: r7 = exec loadmap, r8 = interp (ld.so) loadmap, r9 = GOT (r4_11[3..5]);
	 * r4/5/6/10/11/r12/lr = 0 (the crt _start sets them up); r0 = 0 (static fini = NULL); pc = entry. */
	struct lxp_resume_ctx c;
	memset(&c, 0, sizeof(c));
	c.r4_11[3] = prog->is_fdpic ? (uint32_t)prog->loadmap : 0u;
	c.r4_11[4] = prog->is_fdpic ? (uint32_t)prog->interp_loadmap : 0u;
	c.r4_11[5] = prog->is_fdpic ? (uint32_t)prog->got : 0u;
	c.sp = (uint32_t)sp;
	c.pc = (uint32_t)entry;
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	g_region_exec[sidx] =
		(uint8_t)prog->region_exec; /* RWX region for a copied-text (remote) exec */
#endif
	struct resume_desc *d = stash_desc(sidx, &c, 0);
	volatile struct lnx_fault_diag *diag = &g_lxp_fault_diag[sidx];
	diag->last_spawn_sp = c.sp;
	diag->last_spawn_pc = c.pc;
	diag->last_desc = (uint32_t)(uintptr_t)d;
	diag->last_ridx = (uint32_t)ridx;
	diag->last_kind = 1u;
	int rc = freertos_spawn_common(sidx, ridx, d);
	if (rc == 0)
		g_task_generation[sidx] = generation;
	return rc;
}

static int freertos_spawn_resume(int sidx, uint32_t generation, int ridx,
				  const struct lxp_resume_ctx *ctx, long r0val)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || generation == 0)
		return -1;
	struct resume_desc *d = g_park_desc[sidx];
	int persistent = !g_lxp_used[sidx] && g_tid[sidx] && d;
	if (persistent) {
		if (g_task_generation[sidx] != generation)
			return -1;
		d->r0 = (uint32_t)r0val;
		d->ctx = *ctx;
		__atomic_store_n(&d->ready, 1u, __ATOMIC_RELEASE);
	} else {
		d = stash_desc(sidx, ctx, r0val);
	}
	volatile struct lnx_fault_diag *diag = &g_lxp_fault_diag[sidx];
	diag->last_spawn_sp = ctx->sp;
	diag->last_spawn_pc = ctx->pc;
	diag->last_desc = (uint32_t)(uintptr_t)d;
	diag->last_ridx = (uint32_t)ridx;
	diag->last_kind = persistent ? 3u : 2u;
	if (persistent) {
		vTaskResume(g_tid[sidx]);
		return 0;
	}
	if (g_tid[sidx])
		return -1;
	int rc = freertos_spawn_common(sidx, ridx, d);
	if (rc == 0)
		g_task_generation[sidx] = generation;
	return rc;
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && (portUSING_MPU_WRAPPERS == 1)
/* Give the coordinator (this run-loop task) a Normal-cacheable MPU view of guest region
 * `ridx`'s program region + dyn_pool while it services that slot's DEFERRED syscall or
 * parked-op retry, so the coordinator's CPU reads/writes of the guest's buffers hit the
 * SAME (PIPT) D-cache lines the guest uses — coherent by construction, no per-buffer
 * clean/invalidate. Off this hook the coordinator sees the pools through the uncached
 * background map. Uses the coordinator's own configurable regions 0 and 1, which are
 * unused on this non-restricted task; guests reprogram all configurable regions from
 * their own TCB on switch-in, so this never leaks into a guest's view. The framebuffer
 * (0xC0000000) and the ETH TX bounce (0xC07FF800) sit OUTSIDE the pools → they keep the
 * background non-cacheable attributes and stay DMA/scanout-safe. */
static int g_coord_mapped_ridx = -1;

/* The persistent bounded, non-cacheable window over the memory-mapped QUADSPI NOR (0x90000000):
 * rootfs_window installs it on the coordinator's one spare configurable region (2 — coord_map
 * owns 0 and 1). Recorded here so coord_map RE-SPECIFIES region 2 in the TCB on every remap and
 * never drops it: without this the first coord_map would zero region 2, and the loader's next NOR
 * read (launch() reading an FDPIC ELF straight from QSPI) would fall through to the oversized
 * cacheable PRIVDEFENA background — the exact burst/speculation hazard the window prevents. Zero
 * base = not installed (no QSPI rootfs), so the preserve step below is a no-op. */
static uint32_t g_qspi_win_base;
static uint32_t g_qspi_win_len;
static uint32_t g_qspi_win_par;

static void freertos_coord_map(int ridx)
{
	if (ridx < 0 || ridx >= LXP_NREG)
		return;
	if (!(SCB->CCR & SCB_CCR_DC_Msk))
		return; /* D-cache off: coordinator and guest already agree through SDRAM */
	if (ridx == g_coord_mapped_ridx)
		return; /* already live; the TCB copy restores it across a preemption */

	/* Normal WBWA cacheable, non-shareable, RW, execute-never — the exact attributes
	 * freertos_spawn_common gives the guest's own view of these pools (0x0B). */
	const uint32_t attr = portMPU_REGION_READ_WRITE | portMPU_REGION_EXECUTE_NEVER |
			      (0x0Bu << portMPU_RASR_TEX_S_C_B_LOCATION);
	/* PMSAv7 RASR: ENABLE=bit0, SIZE field=log2(bytes)-1 in bits[5:1]. The pool arrays
	 * are size-aligned (see prog_regions/dyn_pools), so each base is region-aligned. */
	const uint32_t prog_rasr =
		1u | (((uint32_t)(31 - __builtin_clz((unsigned)LXP_PROG_REGION_SIZE)) - 1u) << 1) |
		attr;
	const uint32_t dyn_rasr =
		1u | (((uint32_t)(31 - __builtin_clz((unsigned)LXP_DYN_POOL_SIZE)) - 1u) << 1) |
		attr;

	/* Record in the TCB FIRST so a preemption mid-service restores this same mapping
	 * (the port reprograms configurable regions from the TCB on switch-in); then write
	 * the live registers for immediate effect. Region 2 carries the persistent QSPI NC
	 * window (g_qspi_win_*, installed by rootfs_window) — re-specify it so this remap
	 * preserves it in the TCB rather than zeroing it (the loader reads the NOR through it).
	 * Only regions 0 and 1 are written live below, so the live region 2 is untouched. */
	MemoryRegion_t regions[portNUM_CONFIGURABLE_REGIONS];
	memset(regions, 0, sizeof(regions));
	regions[0].pvBaseAddress = prog_regions[ridx];
	regions[0].ulLengthInBytes = LXP_PROG_REGION_SIZE;
	regions[0].ulParameters = attr;
	regions[1].pvBaseAddress = dyn_pools[ridx];
	regions[1].ulLengthInBytes = LXP_DYN_POOL_SIZE;
	regions[1].ulParameters = attr;
	if (g_qspi_win_base) {
		regions[2].pvBaseAddress = (void *)(uintptr_t)g_qspi_win_base;
		regions[2].ulLengthInBytes = g_qspi_win_len;
		regions[2].ulParameters = g_qspi_win_par;
	}
	vTaskAllocateMPURegions(NULL, regions);

	MPU->RBAR = ((uint32_t)(uintptr_t)prog_regions[ridx]) | MPU_RBAR_VALID_Msk |
		    (portFIRST_CONFIGURABLE_REGION + 0u);
	MPU->RASR = prog_rasr;
	MPU->RBAR = ((uint32_t)(uintptr_t)dyn_pools[ridx]) | MPU_RBAR_VALID_Msk |
		    (portFIRST_CONFIGURABLE_REGION + 1u);
	MPU->RASR = dyn_rasr;
	__DSB();
	__ISB();

	g_coord_mapped_ridx = ridx;
}
#endif

/* Worst tramp-stack usage seen across all guest slots this run, for the R9 stack audit. A task's
 * high-water mark is lost when it is deleted and each slot's static tramp stack is refilled when
 * reused, so the peak is captured here at abort and kept as a running max; ove_lnx_slot_stack_hwm()
 * also folds in any slot still live at the call. This bounds the entry PROLOGUE only — a guest runs
 * on its own stack inside its arena region, not on this FreeRTOS task stack. */
static size_t g_slot_stack_used_max;

static void slot_sample_stack(int sidx)
{
	if (!g_tid[sidx])
		return;
	/* uxTaskGetStackHighWaterMark returns the minimum free stack ever seen, in words. */
	UBaseType_t free_words = uxTaskGetStackHighWaterMark(g_tid[sidx]);
	size_t used = (size_t)(TRAMP_STACK_WORDS - free_words) * sizeof(StackType_t);
	if (used > g_slot_stack_used_max)
		g_slot_stack_used_max = used;
}

/* Bytes used at the high-water mark by the deepest guest-slot tramp stack this run (0 if none ran).
 * The app prints it in the teardown stack audit; declared there via a matching extern. */
size_t ove_lnx_slot_stack_hwm(void)
{
	for (int i = 0; i < LXP_NSLOT; i++)
		if (g_tid[i])
			slot_sample_stack(i);
	return g_slot_stack_used_max;
}

static int freertos_abort_slot(int sidx, uint32_t generation)
{
	if (sidx < 0 || sidx >= LXP_NSLOT)
		return -1;
	if (g_tid[sidx] && g_task_generation[sidx] != generation)
		return -1;
	if (g_tid[sidx]) {
		slot_sample_stack(
			sidx); /* capture the HWM before the task (and its mark) is gone */
		vTaskDelete(g_tid[sidx]);
	}
	g_tid[sidx] = NULL;
	g_park_desc[sidx] = NULL;
	g_task_generation[sidx] = 0;
	return 0;
}

static int freertos_park_slot(int sidx, uint32_t generation)
{
	if (sidx < 0 || sidx >= LXP_NSLOT || !g_lxp_used[sidx] ||
	    !g_tid[sidx] || g_task_generation[sidx] != generation)
		return -1;
	vTaskSuspend(g_tid[sidx]);
	return 0;
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
static void LXP_FAULT_GPR_ONLY freertos_event_post(void)
{
	BaseType_t woken = pdFALSE;
	if (g_ev)
		xSemaphoreGiveFromISR(g_ev, &woken);
	/* Pend a context switch if the (higher-priority) coordinator was woken: event_post runs
	 * from the SVC/MemManage handler (e.g. a program's exit park_frame). Without this yield the
	 * woken coordinator does NOT preempt, so an EXITED unprivileged restricted task keeps spinning
	 * in lxp_park_loop and is context-switched (corrupting its saved registers under the MPU
	 * port) before the coordinator reaps it. Yielding reaps it promptly, before any such switch. */
	portYIELD_FROM_ISR(woken);
}
static void freertos_event_wait(unsigned ms)
{
	if (g_ev)
		xSemaphoreTake(g_ev, pdMS_TO_TICKS(ms));
}

/* Bridge ove_thread_info -> the module-owned lxp_thread_info (identical layout) so
 * the seam can fill the engine's lxp_thread_info-typed thread_list op. */
static int lxp_seam_thread_list(struct lxp_thread_info *o, size_t m, size_t *n)
{
	_Static_assert(sizeof(struct lxp_thread_info) == sizeof(struct ove_thread_info),
		       "LXP/ove thread snapshot ABI mismatch");
	_Static_assert(offsetof(struct lxp_thread_info, identity) ==
			       offsetof(struct ove_thread_info, identity),
		       "LXP/ove thread identity offset mismatch");
	_Static_assert(offsetof(struct lxp_thread_info, lxp_slot) ==
			       offsetof(struct ove_thread_info, lxp_slot),
		       "LXP/ove thread slot offset mismatch");
	size_t local_n = 0;
	size_t *written = n ? n : &local_n;
	int rc = ove_thread_list((struct ove_thread_info *)o, m, written);
	size_t count = *written < m ? *written : m;
	for (size_t i = 0; i < count; i++) {
		o[i].lxp_slot = LXP_THREAD_SLOT_NONE;
		for (int s = 0; s < LXP_NSLOT; s++)
			if (o[i].identity == (uintptr_t)g_tid[s]) {
				o[i].lxp_slot = s;
				break;
			}
	}
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
	"FreeRTOS " tskKERNEL_VERSION_NUMBER " ove-" OVE_BUILD_OVERTOS_REV " lxp-" OVE_BUILD_LXP_REV
_Static_assert(sizeof(LXP_SYSTEM_VERSION) <= 65u, "uname version exceeds Linux utsname field");
static const char *lxp_seam_system_version(void)
{
	return LXP_SYSTEM_VERSION;
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static int freertos_random_fill(void *buf, size_t len)
{
	/* ove_err_t and lxp_err_t values are ABI-pinned identical. */
	return bsp_random_fill(buf, len);
}
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
/* Deterministic, explicitly non-cryptographic provider for the development-only
 * QEMU target. Production hardware must supply a trustworthy port callback. */
static int freertos_random_fill(void *buf, size_t len)
{
	static uint32_t state = 0x6f766572u;
	uint8_t *out = buf;
	for (size_t i = 0; i < len; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		out[i] = (uint8_t)(state >> 24);
	}
	return LXP_OK;
}
#endif

/* Per-run bring-up (was the body of the old lxp_run() wrapper): create the
 * coordinator wakeup semaphore in thread context and enable Bus/UsageFault so a
 * program's fault is contained by our handlers instead of escalating to HardFault
 * (the MPU port's prvSetupMPU only turns on MEMFAULTENA). Invoked by the module's
 * lxp_run() via g_lxp_host_engine.prepare before the run loop. */
static int freertos_prepare(void)
{
	if (!g_ev)
		g_ev = xSemaphoreCreateBinaryStatic(&g_ev_buf);
	/* SHCSR @ 0xE000ED24: BUSFAULTENA = bit 17, USGFAULTENA = bit 18. */
	*(volatile uint32_t *)0xE000ED24u |= (1u << 17) | (1u << 18);
	return 0;
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static void freertos_cache_clean(const void *base, size_t len);
static void freertos_cache_invalidate(const void *base, size_t len);
#endif
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI) && defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && \
	(portUSING_MPU_WRAPPERS == 1)
static void freertos_rootfs_window(const void *base, size_t len);
#endif

const lxp_os_ops_t g_lxp_host_engine = {
	.abi_version = LXP_OS_OPS_ABI_VERSION,
	.struct_size = sizeof(lxp_os_ops_t),
	.prepare = freertos_prepare,
	.region = freertos_region,
	.dyn_pool = freertos_dyn_pool,
	.exec_capture = freertos_exec_capture,
	.spawn_launch = freertos_spawn_launch,
	.spawn_resume = freertos_spawn_resume,
	.abort_slot = freertos_abort_slot,
	.park_prepare = freertos_park_prepare,
	.park_slot = freertos_park_slot,
	.sleep_ms = freertos_sleep_ms,
	.crit_enter = freertos_crit_enter,
	.crit_exit = freertos_crit_exit,
	.event_post = freertos_event_post,
	.event_wait = freertos_event_wait,
	/* OS-service ops (host adapter): the personality core reaches these through
	 * lxp_time_us/ns, lxp_thread_list, lxp_cache_clean/invalidate. */
	.time_us = ove_time_get_us,
	.time_ns = ove_time_get_ns,
	.thread_list = lxp_seam_thread_list,
	.mem_stats = lxp_seam_mem_stats,
	.system_version = lxp_seam_system_version,
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	.cache_clean = freertos_cache_clean,
	.cache_invalidate = freertos_cache_invalidate,
#endif
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && (portUSING_MPU_WRAPPERS == 1)
	.coord_map =
		freertos_coord_map, /* coherent coordinator view of the serviced slot's pools */
#endif
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI) && defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && \
	(portUSING_MPU_WRAPPERS == 1)
	.rootfs_window = freertos_rootfs_window,
#endif
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	.exec_stage = freertos_exec_stage,
#endif
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) || defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
	.random_fill = freertos_random_fill,
#endif
};

#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI) && defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && \
	(portUSING_MPU_WRAPPERS == 1)
/* The rootfs.cpio is XIP'd from the memory-mapped QUADSPI NOR at 0x90000000.  The coordinator —
 * THIS task: it runs lxp_cpio_to_rootfs + the FDPIC loader — is a PRIVILEGED, non-restricted
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
static void freertos_rootfs_window(const void *base, size_t len)
{
	const uint32_t par =
		portMPU_REGION_PRIVILEGED_READ_ONLY | portMPU_REGION_EXECUTE_NEVER |
		(0x08u << portMPU_RASR_TEX_S_C_B_LOCATION); /* TEX=001,S/C/B=0 = Normal NC */
	/* Configurable region 2 — the coordinator's one spare (coord_map owns 0 and 1). Record the
	 * window so coord_map re-specifies region 2 on every remap and never drops it. */
	g_qspi_win_base = (uint32_t)(uintptr_t)base;
	g_qspi_win_len = (uint32_t)len;
	g_qspi_win_par = par;
	MemoryRegion_t regions[portNUM_CONFIGURABLE_REGIONS] = {0};
	regions[2].pvBaseAddress = (void *)(uintptr_t)base;
	regions[2].ulLengthInBytes = (uint32_t)len;
	regions[2].ulParameters = par;
	/* Record it in this task's TCB (configurable region 2) so PendSV re-applies it on every
	 * context switch back to the coordinator — persistent for the whole coordinator life. */
	vTaskAllocateMPURegions(NULL, regions);
	/* vTaskAllocateMPURegions only updates the TCB; the live MPU is not reprogrammed until the
	 * next context switch.  The very next thing the coordinator does is read the NOR (the cpio
	 * parse), which needs the bounded NC view immediately — so program configurable region 2
	 * into the hardware MPU by hand, with the same encoding the port uses on a switch.  Doing it
	 * directly (not via taskYIELD) avoids forcing a context switch from here, which for this task
	 * would be its first switch and trips the FreeRTOS stack-overflow guard. */
	unsigned l2 =
		31u - (unsigned)__builtin_clz((unsigned)len); /* log2(len); len is a power of 2 */
	volatile uint32_t *const mpu_rbar = (volatile uint32_t *)0xE000ED9Cu;
	volatile uint32_t *const mpu_rasr = (volatile uint32_t *)0xE000EDA0u;
	*mpu_rbar = (uint32_t)(uintptr_t)base | (1u << 4) /* VALID */ | 2u /* region 2 */;
	*mpu_rasr = 1u /* ENABLE */ | ((l2 - 1u) << 1) /* SIZE field */ | par;
	__asm__ volatile("dsb 0xf\n\tisb 0xf" ::: "memory");
}
#endif

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Strong override of the engine-common weak no-op (modules/lxp/src/lxp_run.c).
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
 * The STM32Cube CMSIS helper requires a 32-byte-aligned address and does not extend an unaligned
 * final line, so normalize both ends here.  Clean is non-destructive; touching the adjacent bytes
 * in the first/last cache line is safe. */
static void freertos_cache_clean(const void *base, size_t len)
{
	if (!len)
		return;
	uintptr_t addr = (uintptr_t)base;
	if (addr > UINTPTR_MAX - (len - 1u))
		return;
	uintptr_t start = addr & ~(uintptr_t)31u;
	uintptr_t end = ((addr + len - 1u) & ~(uintptr_t)31u) + 32u;
	SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}

static void freertos_cache_invalidate(const void *base, size_t len)
{
	if (!len)
		return;
	uintptr_t addr = (uintptr_t)base;
	if (addr > UINTPTR_MAX - (len - 1u))
		return;
	uintptr_t start = addr & ~(uintptr_t)31u;
	uintptr_t end = ((addr + len - 1u) & ~(uintptr_t)31u) + 32u;
	/* CLEAN+invalidate: the callers (getrandom entropy, vfork data-isolation restore) have the
	 * coordinator WRITE guest memory, then hand it to the guest. With freertos_coord_map that
	 * write is cacheable, so the bytes can sit in dirty D-cache lines — write them back to SDRAM
	 * before dropping, or a plain invalidate would discard the coordinator's just-written data.
	 * When the coordinator's view is uncached the clean is a no-op (nothing dirty) and this is the
	 * old invalidate. The written region is freshly produced by the coordinator, so there is no
	 * stale-tenant line for the clean to push back over live data. */
	SCB_CleanInvalidateDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}
#endif

/* The public lxp_run() now lives in the module (src/lxp_run.c): it publishes the
 * net/display ops and brackets the run loop with g_lxp_host_engine.prepare() /
 * .teardown(). This seam supplies only the engine vtable (g_lxp_host_engine). */
