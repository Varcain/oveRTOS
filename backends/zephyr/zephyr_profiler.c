/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Zephyr sampling profiler backend (Cortex-M).
 *
 * Shape mirrors backends/freertos/freertos_profiler.c:
 *
 *   - Sampling runs in ISR context so PSP points at the interrupted
 *     task's hardware-stacked exception frame: a k_timer whose expiry
 *     function runs out of the system-clock interrupt handler (before
 *     any context switch). Reading PSP there gives us the interrupted
 *     thread's stack, not the timer's. Samples therefore land on a tick
 *     boundary, before the threads that tick wakes have run. If the
 *     system-clock ISR preempted another handler (RETTOBASE == 0), the
 *     frame is on the MSP instead; that path emits a tid-0 sample tagged
 *     with the interrupted handler's exception number ("[ISR n]" in the
 *     UI).
 *
 *   - Multi-frame unwinding is delegated to
 *     backends/common/ove_arm_backtrace.c — the same five-filter chain
 *     FreeRTOS uses (Thumb bit, text range, bl/blx precedes LR,
 *     saved-r7 value window, ascending chain). We only pass backend-
 *     specific constants:
 *       - text bounds: [_image_text_start, _image_text_end) from
 *         Zephyr's linker-defs. These are placed well past the ISR
 *         vector table so low-integer stack values with bit 0 set
 *         can't alias fake offsets.
 *       - stack bounds: the interrupted thread's stack_info, from
 *         start up to its initial stack pointer (start + size - delta),
 *         which excludes the TLS block and any random stack offset
 *         above it. prj.conf.j2 sets CONFIG_THREAD_STACK_INFO; without
 *         it, a short scan within CONFIG_SRAM_BASE_ADDRESS ..
 *         +CONFIG_SRAM_SIZE.
 *
 *   - Stacked-LR fallback mirrors FreeRTOS: the hardware-stacked LR
 *     is used as the second frame ONLY when the scan found no saved
 *     {r7, lr} pair. For non-leaf functions the stacked LR is an
 *     address inside the function itself (return point from an internal
 *     `bl`), not a caller — using it there would inject phantom
 *     self-edges into the flame graph.
 *
 *   - Symbolication is host-side (same as FreeRTOS) via the dashboard
 *     bridge — arm-none-eabi-nm on the Zephyr ELF.
 *
 * Rate control: the k_timer runs one-shot and is re-armed from its expiry
 * with a period jittered around the mean (1 / rate) by +/-25%, so the
 * sampler cannot phase-lock with periodic work; set_rate() changes the
 * mean for the next re-arm.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <stdint.h>
#include <string.h>

#include <cmsis_core.h>
#include <zephyr/kernel.h>

#include "ove/ove_arm_backtrace.h"
#include "ove/profiler.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/types.h"
#include "ove_profiler_ring.h"

#ifndef CONFIG_OVE_PROFILER_HZ
#define CONFIG_OVE_PROFILER_HZ 250
#endif

/* SRAM bounds. Derived from Zephyr Kconfig so the same profiler works
 * on any Cortex-M board without touching this file. CONFIG_SRAM_SIZE is
 * in KB, CONFIG_SRAM_BASE_ADDRESS is a byte address. Only used when the
 * thread's stack range is unknown, with a short scan window. */
#define OVE_SRAM_BASE ((uintptr_t)CONFIG_SRAM_BASE_ADDRESS)
#define OVE_SRAM_END ((uintptr_t)CONFIG_SRAM_BASE_ADDRESS + (uintptr_t)CONFIG_SRAM_SIZE * 1024U)
#define OVE_FALLBACK_SCAN_BYTES 1024u

/* Text section bounds from Zephyr's linker script (include/zephyr/linker/
 * linker-defs.h). __text_region_start sits after the ISR vector table so
 * small stack values with bit 0 set can't pass the is-text filter as
 * fake vector offsets. */
extern char __text_region_start[];
extern char __text_region_end[];
#define OVE_TEXT_BASE ((uintptr_t)__text_region_start)
#define OVE_TEXT_END ((uintptr_t)__text_region_end)

/* Plain int + __atomic_* builtins rather than C11 _Atomic types: the
 * Zephyr SDK gcc's stdatomic.h macros use a temporary `_Atomic int`
 * tmp whose address is passed to __atomic_load — clang-tidy trips on
 * the "address argument to atomic op must be pointer to trivially-
 * copyable type" check during cross-compile lint.  The builtins give
 * identical codegen on Cortex-M (aligned 4-byte load/store is a single
 * instruction). Same approach in backends/freertos/freertos_profiler.c. */
static int profiler_running;
static struct k_timer profiler_timer;

static inline uint32_t read_psp(void)
{
	uint32_t psp;
	__asm volatile("mrs %0, psp" : "=r"(psp));
	return psp;
}

static inline uint32_t read_msp(void)
{
	uint32_t msp;
	__asm volatile("mrs %0, msp" : "=r"(msp));
	return msp;
}

/* The system-clock ISR preempted another exception handler (PendSV, or a
 * device IRQ of a lower urgency). Its stacked frame is on the
 * MSP; locate it and push a tid-0 sample tagged with the interrupted
 * handler's exception number. The PSP path handles thread-mode samples.
 * SysTick, exception 15, is the sampler itself. */
#define SAMPLER_EXC 15u

static void sample_interrupted_handler(void)
{
	const uint32_t *frame = ove_arm_find_exc_frame(read_msp(), OVE_SRAM_END, OVE_TEXT_BASE,
						       OVE_TEXT_END, 256, SAMPLER_EXC);
	if (!frame)
		return;

	uint32_t ipsr = frame[OVE_ARM_EXC_XPSR] & 0x1FFu;

	struct ove_profiler_sample s;
	memset(&s, 0, sizeof(s));
	s.ts_us = ove_state_stats_now_us();
	s.tid = 0;
	s.state = OVE_THREAD_STATE_RUNNING;
	s.isr = (uint16_t)ipsr;
	s.pcs[0] = (uintptr_t)(frame[OVE_ARM_EXC_PC] & ~1u);
	s.depth = 1;
	(void)ove_profiler_ring_push(&s);
}

/*
 * Called from the k_timer expiry path, which Zephyr dispatches out of
 * the system-clock ISR after the timeout list has been processed. At
 * that point PSP still points at the interrupted thread's stacked
 * exception frame (r0-r3, r12, lr, pc, xpsr); we read PSP[OVE_ARM_EXC_PC]
 * for the leaf PC, then scan above the frame for saved {r7, lr} pairs.
 */
void ove_backend_profiler_on_tick(void)
{
	if (!__atomic_load_n(&profiler_running, __ATOMIC_ACQUIRE))
		return;

	/* RETTOBASE == 0: the system-clock exception is nested on top of
	 * another handler, so the interrupted frame is on MSP, not PSP.
	 * SysTick runs at _IRQ_PRIO_OFFSET, above PendSV. IRQ_CONNECT places
	 * a device IRQ at its devicetree priority plus that offset, so SysTick
	 * preempts the devices at devicetree priority 1 or more (all of them
	 * on the QEMU mps2 boards); a priority-0 device, as most are on the
	 * STM32F7, shares SysTick's level and its time is not attributed. */
	if (!(SCB->ICSR & SCB_ICSR_RETTOBASE_Msk)) {
		sample_interrupted_handler();
		return;
	}

	uint32_t psp = read_psp();
	if (psp == 0)
		return;

	/*
	 * k_current_get returns the interrupted thread when called from
	 * ISR context (it's a plain read of _current_cpu->current).
	 * k_thread_custom_data_get() returns what we stashed in
	 * thread_wrapper(); it's NULL for the idle thread and any system
	 * thread whose custom_data slot was never set — fall back to the
	 * raw k_thread * as tid so those samples aren't silently dropped
	 * (otherwise the dashboard would misattribute idle time).
	 */
	k_tid_t cur = k_current_get();
	if (!cur)
		return;
	struct ove_thread *t = (struct ove_thread *)k_thread_custom_data_get();
	uintptr_t tid = t ? (uintptr_t)t : (uintptr_t)cur;

	const uint32_t *frame = (const uint32_t *)psp;
	/* Clear the Thumb bit so pcs[0] matches the encoding walked frames
	 * use — dashboard symbol resolver expects consistent bit-0. */
	uint32_t pc = frame[OVE_ARM_EXC_PC] & ~1u;

	struct ove_profiler_sample s;
	memset(&s, 0, sizeof(s));
	s.ts_us = ove_state_stats_now_us();
	s.tid = (uint32_t)tid;
	s.state = OVE_THREAD_STATE_RUNNING;
	s.pcs[0] = (uintptr_t)pc;
	s.depth = 1;

	/* Bound the scan by the thread's real stack. A PSP outside it (e.g.
	 * a user thread on its privileged stack) makes the walker return 0
	 * and the stacked-LR fallback below applies. */
	uintptr_t stack_lo = psp;
	uintptr_t stack_hi = psp + OVE_FALLBACK_SCAN_BYTES;
#ifdef CONFIG_THREAD_STACK_INFO
	stack_lo = (uintptr_t)cur->stack_info.start;
	/* start + size - delta is the thread's initial SP: TLS and the
	 * random stack offset sit above it and hold no frames. */
	stack_hi = stack_lo + (uintptr_t)cur->stack_info.size - (uintptr_t)cur->stack_info.delta;
#else
	if (stack_hi > OVE_SRAM_END || stack_lo < OVE_SRAM_BASE)
		stack_hi = stack_lo; /* outside RAM: no scan */
#endif
	int extra = ove_arm_backtrace_walk((uintptr_t)psp, OVE_TEXT_BASE, OVE_TEXT_END, stack_lo,
					   stack_hi, &s.pcs[1], CONFIG_OVE_PROFILER_MAX_DEPTH - 1);
	s.depth = (uint8_t)(1 + extra);

	/*
	 * Fallback: if the scan found no saved LR, the interrupted function
	 * either didn't push {r7, lr} (true leaf), or the timer ISR hit
	 * inside a prologue before the push completed. In either case the
	 * hardware-stacked LR still holds the caller's return address, so
	 * it's the only usable second frame. When the scan DID find at
	 * least one saved-LR, we deliberately drop the stacked LR — for a
	 * non-leaf function that's already returned from an internal `bl`,
	 * stacked LR holds an address *inside* the same function, injecting
	 * a phantom self-edge in the flame graph.
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

/* Mean sampling period (us) and jitter state. Each period is drawn
 * uniformly in [0.75, 1.25) x mean so the sampler cannot phase-lock with
 * periodic work (a fixed-period sampler aligned with a task's period sees
 * it always, or never). */
static uint32_t mean_period_us;
static uint32_t jitter_state = 0x2545F491u;
static bool timer_initialised;

static uint32_t next_period_us(void)
{
	uint32_t x = jitter_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	jitter_state = x;
	uint32_t mean = __atomic_load_n(&mean_period_us, __ATOMIC_RELAXED);
	uint32_t span = mean / 2u;
	uint32_t us = mean - mean / 4u + (span ? x % span : 0u);
	return us ? us : 1u;
}

/* The drawn period in whole ticks, rounded to the nearest one: K_USEC
 * rounds up, which lowered the mean rate by half a tick per period
 * (a third on a 100 Hz tick). */
static k_timeout_t next_period(void)
{
	uint32_t ticks = k_us_to_ticks_near32(next_period_us());
	return K_TICKS(ticks ? ticks : 1u);
}

/* One-shot timer, re-armed from its own expiry with a fresh period. */
static void profiler_timer_expiry(struct k_timer *timer)
{
	if (!__atomic_load_n(&profiler_running, __ATOMIC_ACQUIRE))
		return;
	ove_backend_profiler_on_tick();
	k_timer_start(timer, next_period(), K_NO_WAIT);
}

int ove_backend_profiler_start(void)
{
	if (!timer_initialised) {
		k_timer_init(&profiler_timer, profiler_timer_expiry, NULL);
		timer_initialised = true;
	}
	if (__atomic_load_n(&mean_period_us, __ATOMIC_RELAXED) == 0)
		ove_backend_profiler_set_rate(ove_backend_profiler_get_max_hz());
	__atomic_store_n(&profiler_running, 1, __ATOMIC_RELEASE);
	k_timer_start(&profiler_timer, next_period(), K_NO_WAIT);
	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
	__atomic_store_n(&profiler_running, 0, __ATOMIC_RELEASE);
	if (timer_initialised)
		k_timer_stop(&profiler_timer);
}

/* Takes effect at the next re-arm; never starts a stopped profiler. */
void ove_backend_profiler_set_rate(uint32_t hz)
{
	uint32_t max_hz = ove_backend_profiler_get_max_hz();
	if (hz == 0 || hz > max_hz)
		hz = max_hz;
	uint32_t us = 1000000u / hz;
	__atomic_store_n(&mean_period_us, us ? us : 1u, __ATOMIC_RELAXED);
}

uint32_t ove_backend_profiler_get_flags(void)
{
	/* Samples only the thread interrupted by the timer (on-CPU). */
	return 0;
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	/* Capped by the system clock granularity. At 1 kHz tick rate
	 * anything above 1 kHz collapses onto the tick anyway. */
	uint32_t cfg_hz = (uint32_t)CONFIG_OVE_PROFILER_HZ;
	uint32_t tick_hz = (uint32_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	return (cfg_hz < tick_hz) ? cfg_hz : tick_hz;
}

#endif /* CONFIG_OVE_PROFILER */
