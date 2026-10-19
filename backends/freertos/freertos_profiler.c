/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * FreeRTOS sampling profiler backend (Cortex-M).
 *
 * Sampling runs in the interrupt of a dedicated hardware timer (CMSDK APB
 * TIMER1 on MPS2-AN500): the hardware has stacked the interrupted task's
 * exception frame onto its PSP, so we read the stacked PC from there,
 * walk the stack and push a sample.
 *
 * Why not the kernel tick: sampling exactly on the SysTick boundary never
 * sees work that the tick itself wakes (delays, timers, periodic loops) —
 * a thread using 41% of the CPU that way got 0.8% of the samples. The
 * timer runs independently of the tick, and each period is jittered by
 * +/-25% so the sampler cannot phase-lock with periodic work either.
 *
 * Multi-frame unwinding: the stack-scan walker and the five heuristic
 * filters (Thumb bit, text range, bl/blx precedence, saved-r7 adjacency,
 * ascending-chain invariant) live in backends/common/ove_arm_backtrace.c.
 * We only pass the FreeRTOS-specific bounds:
 *   - text bounds: [__stext, __etext) from the linker script
 *     (__stext is placed right after the ISR vector table so small
 *      integer stack values with the Thumb bit set can't alias
 *      __isr_vector+0x2 style fake offsets)
 *   - the interrupted task's stack [pxStack, pxEndOfStack], recorded at
 *     task creation by the traceTASK_CREATE hook (FreeRTOSConfig.h.j2);
 *     tasks missing from the table fall back to a short scan within the
 *     MPS2-AN500 RAM window [0x20000000, 0x20400000).
 * The hardware-stacked LR is used only as a fallback when the scan
 * finds no saved {r7, lr} pair (true-leaf functions or prologue-
 * interrupted samples); see gotcha 24 in memory/project_profiler_trace.md.
 *
 * Symbolication: host-side via the dashboard bridge (arm-none-eabi-nm
 * on the firmware ELF).
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ove/ove_arm_backtrace.h"
#include "ove/profiler.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/types.h"
#include "ove_profiler_ring.h"

#ifndef CONFIG_OVE_PROFILER_HZ
#define CONFIG_OVE_PROFILER_HZ 250
#endif

/* SRAM bounds. MPS2-AN500 linker script puts RAM at 0x20000000 + 4 MB
 * (boards/qemu-mps2-an500/freertos/mps2_an500.ld). Only used for tasks
 * whose stack range is unknown, with a short scan window. */
#define OVE_SRAM_BASE 0x20000000u
#define OVE_SRAM_END 0x20400000u
#define OVE_FALLBACK_SCAN_BYTES 1024u

/* Text section bounds. __stext is placed right after the ISR vector
 * table in mps2_an500.ld so integer stack values with bit 0 set
 * (0x3, 0x5, 0x23, 0x27, ...) can't pass the is-text filter as fake
 * offsets into __isr_vector. __etext is the linker-defined end of .text. */
extern uint32_t __stext;
extern uint32_t __etext;
#define OVE_TEXT_BASE ((uintptr_t)&__stext)
#define OVE_TEXT_END ((uintptr_t)&__etext)

/* ── Per-task stack ranges ────────────────────────────────────────
 * Filled from traceTASK_CREATE / traceTASK_DELETE, which run inside the
 * kernel's task-list critical section. The timer sampler reads the table
 * from an ISR at the kernel interrupt priority, which that critical section
 * masks, so it always sees a consistent table. Covers kernel tasks (IDLE,
 * Tmr Svc) as well as ove threads. */
#define OVE_PROFILER_MAX_TASKS 32

struct task_stack {
	TaskHandle_t task;
	uintptr_t lo; /* inclusive */
	uintptr_t hi; /* exclusive */
};

static struct task_stack task_stacks[OVE_PROFILER_MAX_TASKS];
static unsigned int task_stack_count;

void ove_backend_profiler_task_created(void *task, void *stack_lo, void *stack_end)
{
	if (!task || task_stack_count >= OVE_PROFILER_MAX_TASKS)
		return;
	struct task_stack *e = &task_stacks[task_stack_count++];
	e->task = (TaskHandle_t)task;
	e->lo = (uintptr_t)stack_lo;
	/* pxEndOfStack is the highest valid stack word (inclusive). */
	e->hi = (uintptr_t)stack_end + sizeof(StackType_t);
}

void ove_backend_profiler_task_deleted(void *task)
{
	for (unsigned int i = 0; i < task_stack_count; i++) {
		if (task_stacks[i].task != (TaskHandle_t)task)
			continue;
		task_stacks[i] = task_stacks[--task_stack_count];
		return;
	}
}

static int task_stack_find(TaskHandle_t task, uintptr_t *lo, uintptr_t *hi)
{
	for (unsigned int i = 0; i < task_stack_count; i++) {
		if (task_stacks[i].task == task) {
			*lo = task_stacks[i].lo;
			*hi = task_stacks[i].hi;
			return 1;
		}
	}
	return 0;
}

/* Plain int + __atomic_* builtins rather than C11 _Atomic types: the
 * Zephyr SDK gcc's stdatomic.h macros use a temporary `_Atomic int`
 * tmp whose address is then passed to __atomic_load — clang-tidy
 * trips on the "address argument to atomic op must be pointer to
 * trivially-copyable type" check during cross-compile lint.  The
 * builtins give identical codegen on Cortex-M (aligned 4-byte
 * load/store are single-instruction). Same approach in
 * backends/zephyr/zephyr_profiler.c. */
static int profiler_running;

/* ── Sampling timer: CMSDK APB TIMER1 (MPS2-AN500) ───────────────────
 * 32-bit down-counter clocked at the system clock; interrupts and
 * reloads from RELOAD when it reaches zero. IRQ 9, named in the board's
 * vector table (startup_CMSDK_CM7.s). Its priority is the kernel's (the
 * same as SysTick and PendSV), so it never preempts a context switch or a
 * critical section: pxCurrentTCB and PSP always describe the same task. */
#define PROF_TIMER_BASE 0x40001000u
#define PROF_TIMER_IRQ 9u
#define PROF_TIMER_CTRL (*(volatile uint32_t *)(PROF_TIMER_BASE + 0x00u))
#define PROF_TIMER_VALUE (*(volatile uint32_t *)(PROF_TIMER_BASE + 0x04u))
#define PROF_TIMER_RELOAD (*(volatile uint32_t *)(PROF_TIMER_BASE + 0x08u))
#define PROF_TIMER_INTCLEAR (*(volatile uint32_t *)(PROF_TIMER_BASE + 0x0Cu))
#define PROF_TIMER_CTRL_EN (1u << 0)
#define PROF_TIMER_CTRL_IRQEN (1u << 3)
#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define NVIC_ICER0 (*(volatile uint32_t *)0xE000E180u)
#define NVIC_ICPR0 (*(volatile uint32_t *)0xE000E280u)
#define NVIC_IPR_BYTE(irq) (*(volatile uint8_t *)(0xE000E400u + (irq)))
#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04u)
#define SCB_ICSR_RETTOBASE (1u << 11)

extern uint32_t SystemCoreClock;

/* Mean sampling period in timer cycles (SystemCoreClock / Hz). */
static uint32_t mean_period;
static uint32_t jitter_state = 0x2545F491u;

/* Next period: uniform in [0.75, 1.25) x mean (xorshift32). */
static uint32_t next_period(void)
{
	uint32_t x = jitter_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	jitter_state = x;
	uint32_t mean = __atomic_load_n(&mean_period, __ATOMIC_RELAXED);
	uint32_t span = mean / 2u;
	return mean - mean / 4u + (span ? x % span : 0u);
}

static inline uint32_t read_psp(void)
{
	uint32_t psp;
	__asm volatile("mrs %0, psp" : "=r"(psp));
	return psp;
}

/* Take one sample of the interrupted task (timer ISR context). PSP points
 * at its stacked exception frame (r0-r3, r12, lr, pc, xpsr). */
static void sample_interrupted_task(void)
{
	/* Only thread-mode code can be attributed through PSP: skip if we
	 * interrupted another handler, or before the scheduler starts. */
	if (!(SCB_ICSR & SCB_ICSR_RETTOBASE))
		return;
	uint32_t psp = read_psp();
	if (psp == 0)
		return;

	/* xTaskGetCurrentTaskHandle() is a plain read of pxCurrentTCB and is
	 * safe from ISR. xTaskGetApplicationTaskTag() takes a non-ISR
	 * critical section (taskENTER_CRITICAL), which trips configASSERT
	 * on Cortex-M when called from an ISR — use the FromISR variant. */
	TaskHandle_t h = xTaskGetCurrentTaskHandle();
	if (!h)
		return;
	struct ove_thread *t = (struct ove_thread *)xTaskGetApplicationTaskTagFromISR(h);
	uintptr_t tid = t ? (uintptr_t)t : (uintptr_t)h;

	const uint32_t *frame = (const uint32_t *)psp;
	/* Normalise the leaf PC: hardware stacks it with the Thumb bit set,
	 * but walked saved-LRs below are stored with bit 0 cleared so the
	 * dashboard's symbol resolver sees a consistent encoding across all
	 * frames. Leaving it mixed makes pc-vs-lr compares unreliable and
	 * can leak the stray low bit into hex fallbacks. */
	uint32_t pc = frame[OVE_ARM_EXC_PC] & ~1u;

	struct ove_profiler_sample s;
	memset(&s, 0, sizeof(s));
	s.ts_us = ove_state_stats_now_us();
	s.tid = (uint32_t)tid;
	s.state = OVE_THREAD_STATE_RUNNING;
	s.pcs[0] = (uintptr_t)pc;
	s.depth = 1;

	uintptr_t stack_lo, stack_hi;
	if (!task_stack_find(h, &stack_lo, &stack_hi)) {
		stack_lo = psp;
		stack_hi = psp + OVE_FALLBACK_SCAN_BYTES;
		if (stack_hi > OVE_SRAM_END || stack_lo < OVE_SRAM_BASE)
			stack_hi = stack_lo; /* outside RAM: no scan */
	}
	int extra = ove_arm_backtrace_walk((uintptr_t)psp, OVE_TEXT_BASE, OVE_TEXT_END, stack_lo,
					   stack_hi, &s.pcs[1], CONFIG_OVE_PROFILER_MAX_DEPTH - 1);
	s.depth = (uint8_t)(1 + extra);

	/*
	 * Fallback: if the scan found no saved LR, the interrupted function
	 * either didn't push {r7, lr} (true leaf — it never called anything),
	 * or the interrupt hit inside a prologue before the push completed.
	 * In that case the hardware-stacked LR still holds the caller's
	 * return address, so it's the only usable second frame — include it.
	 *
	 * When the scan DID find at least one saved-LR, we intentionally
	 * drop the stacked LR. For a non-leaf function that has already
	 * executed and returned from an internal `bl`, stacked LR holds an
	 * address *inside* that same function (the return point from the
	 * internal call) — not a caller. Including it would inject a
	 * phantom self-edge between leaf and true caller in the flame
	 * graph, making call chains look like "main → foo → foo → leaf".
	 * The scanned saved-LR from foo's prologue is authoritative.
	 */
	if (s.depth == 1) {
		uint32_t stacked_lr = frame[OVE_ARM_EXC_LR] & ~1u;
		if (stacked_lr != 0 && stacked_lr != pc && stacked_lr >= OVE_TEXT_BASE &&
		    stacked_lr < OVE_TEXT_END &&
		    ove_arm_backtrace_lr_is_post_bl((uintptr_t)stacked_lr, OVE_TEXT_BASE,
						    OVE_TEXT_END)) {
			s.pcs[1] = (uintptr_t)stacked_lr;
			s.depth = 2;
		}
	}

	(void)ove_profiler_ring_push(&s);
}

void TIMER1_IRQHandler(void)
{
	PROF_TIMER_INTCLEAR = 1u;
	/* Takes effect at the next reload: each period is drawn anew. */
	PROF_TIMER_RELOAD = next_period();
	if (__atomic_load_n(&profiler_running, __ATOMIC_ACQUIRE))
		sample_interrupted_task();
}

int ove_backend_profiler_start(void)
{
	if (__atomic_load_n(&mean_period, __ATOMIC_RELAXED) == 0)
		ove_backend_profiler_set_rate(ove_backend_profiler_get_max_hz());

	__atomic_store_n(&profiler_running, 1, __ATOMIC_RELEASE);
	PROF_TIMER_CTRL = 0u;
	PROF_TIMER_INTCLEAR = 1u;
	PROF_TIMER_RELOAD = next_period();
	PROF_TIMER_VALUE = next_period();
	NVIC_IPR_BYTE(PROF_TIMER_IRQ) = (uint8_t)configKERNEL_INTERRUPT_PRIORITY;
	NVIC_ICPR0 = 1u << PROF_TIMER_IRQ;
	NVIC_ISER0 = 1u << PROF_TIMER_IRQ;
	PROF_TIMER_CTRL = PROF_TIMER_CTRL_EN | PROF_TIMER_CTRL_IRQEN;
	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
	__atomic_store_n(&profiler_running, 0, __ATOMIC_RELEASE);
	PROF_TIMER_CTRL = 0u;
	NVIC_ICER0 = 1u << PROF_TIMER_IRQ;
}

void ove_backend_profiler_set_rate(uint32_t hz)
{
	uint32_t max_hz = ove_backend_profiler_get_max_hz();
	if (hz == 0 || hz > max_hz)
		hz = max_hz;
	uint32_t period = SystemCoreClock / hz;
	__atomic_store_n(&mean_period, period ? period : 1u, __ATOMIC_RELEASE);
}

uint32_t ove_backend_profiler_get_flags(void)
{
	/* Samples only the task interrupted by the sampling timer (on-CPU). */
	return 0;
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	return (uint32_t)CONFIG_OVE_PROFILER_HZ;
}

#endif /* CONFIG_OVE_PROFILER */
