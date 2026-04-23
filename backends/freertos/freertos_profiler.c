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
 * Cortex-M has no in-task signal mechanism, so we can't copy the POSIX
 * "pthread_kill + backtrace() in the target's signal handler" design.
 * Instead, sampling is driven from vApplicationTickHook — the FreeRTOS
 * tick hook runs in SysTick ISR context at configTICK_RATE_HZ, and the
 * hardware has already stacked the interrupted task's exception frame
 * onto its PSP. We read the stacked PC from there and push a sample.
 *
 * Cross-backend contract:
 *   sample_tick() is called from the sim-debug pump. On POSIX/WASM that
 *   is where sampling is kicked off. On FreeRTOS sampling is ISR-driven,
 *   so sample_tick() is a no-op — the actual producer is
 *   ove_backend_profiler_on_tick(), invoked from freertos_hooks.c.
 *
 * Multi-frame unwinding: the stack-scan walker and the five heuristic
 * filters (Thumb bit, text range, bl/blx precedence, saved-r7 adjacency,
 * ascending-chain invariant) live in backends/common/ove_arm_backtrace.c.
 * We only pass the FreeRTOS-specific constants:
 *   - text bounds: [__stext, __etext) from the linker script
 *     (__stext is placed right after the ISR vector table so small
 *      integer stack values with the Thumb bit set can't alias
 *      __isr_vector+0x2 style fake offsets)
 *   - SRAM bounds: MPS2-AN500 RAM window [0x20000000, 0x20400000)
 *   - fill pattern: 0xA5A5A5A5 from FreeRTOS's tskSTACK_FILL_BYTE.
 * The hardware-stacked LR is used only as a fallback when the scan
 * finds no saved {r7, lr} pair (true-leaf functions or prologue-
 * interrupted samples); see gotcha 24 in memory/project_profiler_trace.md.
 *
 * Symbolication: host-side via the dashboard bridge (arm-none-eabi-nm
 * on the firmware ELF). drain_symbols() returns 0.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <stdatomic.h>
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
 * (boards/qemu-mps2-an500/freertos/mps2_an500.ld). Any candidate outside
 * this range can't point at a live stack frame — stop walking. */
#define OVE_SRAM_BASE 0x20000000u
#define OVE_SRAM_END  0x20400000u

/* Text section bounds. __stext is placed right after the ISR vector
 * table in mps2_an500.ld so integer stack values with bit 0 set
 * (0x3, 0x5, 0x23, 0x27, ...) can't pass the is-text filter as fake
 * offsets into __isr_vector. __etext is the linker-defined end of .text. */
extern uint32_t __stext;
extern uint32_t __etext;
#define OVE_TEXT_BASE ((uintptr_t)&__stext)
#define OVE_TEXT_END  ((uintptr_t)&__etext)

/* FreeRTOS task stack fill pattern (tskSTACK_FILL_BYTE = 0xA5 repeated).
 * Hitting this during the scan means we've reached unused stack. */
#define OVE_STACK_FILL 0xA5A5A5A5u

static atomic_int  profiler_running;

/* Runtime rate control. The tick hook fires at configTICK_RATE_HZ
 * (1 kHz on QEMU MPS2); sample_divisor picks an integer divisor so the
 * actual sampling rate = tick_rate / divisor. divisor == 1 samples every
 * tick. Dashboard → set_rate() → divisor update. */
static atomic_uint sample_divisor = 1;
static atomic_uint sample_counter;

static inline uint32_t read_psp(void)
{
	uint32_t psp;
	__asm volatile("mrs %0, psp" : "=r"(psp));
	return psp;
}

/*
 * Called from vApplicationTickHook (SysTick ISR context).
 *
 * SysTick runs at configKERNEL_INTERRUPT_PRIORITY (lowest), so by the
 * time we get here any higher-priority IRQ has already returned. PSP
 * points at the interrupted task's stack top, where the CPU stacked
 * r0-r3,r12,lr,pc,xpsr on exception entry. We read PSP[EXC_FRAME_PC].
 */
void ove_backend_profiler_on_tick(void)
{
	if (!atomic_load_explicit(&profiler_running, memory_order_acquire))
		return;

	unsigned div = atomic_load_explicit(&sample_divisor,
					    memory_order_relaxed);
	unsigned c = atomic_fetch_add_explicit(&sample_counter, 1u,
					       memory_order_relaxed) + 1u;
	if (c < div)
		return;
	atomic_store_explicit(&sample_counter, 0u, memory_order_relaxed);

	/* The PSP tracks the interrupted task's stack — except when the
	 * tick fires while the scheduler isn't yet running (PSP == 0) or
	 * while we interrupted another exception. Skip those. */
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
	struct ove_thread *t = (struct ove_thread *)
		xTaskGetApplicationTaskTagFromISR(h);
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
	s.tid   = (uint32_t)tid;
	s.state = OVE_THREAD_STATE_RUNNING;
	s.pcs[0] = (uintptr_t)pc;
	s.depth  = 1;

	int extra = ove_arm_backtrace_walk((uintptr_t)psp,
					   OVE_TEXT_BASE, OVE_TEXT_END,
					   OVE_SRAM_BASE, OVE_SRAM_END,
					   OVE_STACK_FILL,
					   &s.pcs[1],
					   CONFIG_OVE_PROFILER_MAX_DEPTH - 1);
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
		if (stacked_lr != 0 && stacked_lr != pc &&
		    stacked_lr >= OVE_TEXT_BASE && stacked_lr < OVE_TEXT_END &&
		    ove_arm_backtrace_lr_is_post_bl((uintptr_t)stacked_lr,
						    OVE_TEXT_BASE, OVE_TEXT_END)) {
			s.pcs[1] = (uintptr_t)stacked_lr;
			s.depth  = 2;
		}
	}

	(void)ove_profiler_ring_push(&s);
}

/*
 * No-op: sampling is ISR-driven via ove_backend_profiler_on_tick().
 * The pump still calls this every iteration per the cross-backend
 * contract — keeping it empty means the pump pays zero per-tick cost
 * on FreeRTOS.
 */
void ove_backend_profiler_sample_tick(void)
{
}

int ove_backend_profiler_start(void)
{
	atomic_store_explicit(&profiler_running, 1, memory_order_release);
	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
	atomic_store_explicit(&profiler_running, 0, memory_order_release);
}

void ove_backend_profiler_set_rate(uint32_t hz)
{
	uint32_t max_hz = ove_backend_profiler_get_max_hz();
	if (hz == 0 || hz > max_hz)
		hz = max_hz;

	uint32_t tick_hz = (uint32_t)configTICK_RATE_HZ;
	unsigned div = (unsigned)(tick_hz / hz);
	if (div == 0)
		div = 1;

	atomic_store_explicit(&sample_divisor, div, memory_order_release);
	atomic_store_explicit(&sample_counter, 0, memory_order_release);
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	uint32_t cfg_hz  = (uint32_t)CONFIG_OVE_PROFILER_HZ;
	uint32_t tick_hz = (uint32_t)configTICK_RATE_HZ;
	/* Can't sample faster than the tick fires. */
	return (cfg_hz < tick_hz) ? cfg_hz : tick_hz;
}

/*
 * FreeRTOS symbolicates host-side: the dashboard bridge feeds
 * unresolved PCs to arm-none-eabi-gdb via MI and caches the result.
 * Matches the POSIX path (nm on the sim binary).
 */
size_t ove_backend_profiler_drain_symbols(char *out, size_t out_max)
{
	(void)out;
	(void)out_max;
	return 0;
}

#endif /* CONFIG_OVE_PROFILER */
