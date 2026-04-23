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
 *     task's hardware-stacked exception frame. FreeRTOS hooks into
 *     vApplicationTickHook; on Zephyr the equivalent is a k_timer whose
 *     expiry function runs out of the system-clock interrupt handler
 *     (before any context switch). Reading PSP there gives us the
 *     interrupted thread's stack, not the timer's.
 *
 *   - Multi-frame unwinding is delegated to
 *     backends/common/ove_arm_backtrace.c — the same five-filter chain
 *     FreeRTOS uses (Thumb bit, text range, bl/blx precedes LR,
 *     saved-r7 adjacency, ascending chain). We only pass backend-
 *     specific constants:
 *       - text bounds: [_image_text_start, _image_text_end) from
 *         Zephyr's linker-defs. These are placed well past the ISR
 *         vector table so low-integer stack values with bit 0 set
 *         can't alias fake offsets.
 *       - SRAM bounds: CONFIG_SRAM_BASE_ADDRESS .. +CONFIG_SRAM_SIZE*1024.
 *       - fill pattern: 0xAAAAAAAA when CONFIG_INIT_STACKS=y (Zephyr
 *         fills stacks with 0xaa); 0 otherwise (disables the check).
 *
 *   - Stacked-LR fallback mirrors FreeRTOS: the hardware-stacked LR
 *     is used as the second frame ONLY when the scan found no saved
 *     {r7, lr} pair. For non-leaf functions the stacked LR is an
 *     address inside the function itself (return point from an internal
 *     `bl`), not a caller — using it there would inject phantom
 *     self-edges into the flame graph. See gotcha 24 in
 *     memory/project_profiler_trace.md.
 *
 *   - Symbolication is host-side (same as FreeRTOS) via the dashboard
 *     bridge — arm-none-eabi-nm on the Zephyr ELF. drain_symbols() is 0.
 *
 * Rate control: unlike FreeRTOS's tick-hook + divisor, the Zephyr path
 * re-arms the k_timer with a new period in set_rate(). k_timer_start on
 * a running timer restarts it, so this is atomic from the caller's POV.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

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
 * in KB, CONFIG_SRAM_BASE_ADDRESS is a byte address. */
#define OVE_SRAM_BASE ((uintptr_t)CONFIG_SRAM_BASE_ADDRESS)
#define OVE_SRAM_END  ((uintptr_t)CONFIG_SRAM_BASE_ADDRESS + \
		       (uintptr_t)CONFIG_SRAM_SIZE * 1024U)

/* Text section bounds from Zephyr's linker script (include/zephyr/linker/
 * linker-defs.h). __text_region_start sits after the ISR vector table so
 * small stack values with bit 0 set can't pass the is-text filter as
 * fake vector offsets. */
extern char __text_region_start[];
extern char __text_region_end[];
#define OVE_TEXT_BASE ((uintptr_t)__text_region_start)
#define OVE_TEXT_END  ((uintptr_t)__text_region_end)

/* Stack fill pattern. Zephyr fills every new thread stack with 0xaa
 * when CONFIG_INIT_STACKS=y — hitting 0xAAAAAAAA during the scan means
 * we've walked off the live portion of the stack. Without INIT_STACKS
 * the scan relies purely on SRAM bounds + scan-word cap. */
#ifdef CONFIG_INIT_STACKS
#define OVE_STACK_FILL 0xAAAAAAAAu
#else
#define OVE_STACK_FILL 0u
#endif

static atomic_int profiler_running;
static struct k_timer profiler_timer;

static inline uint32_t read_psp(void)
{
	uint32_t psp;
	__asm volatile("mrs %0, psp" : "=r"(psp));
	return psp;
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
	if (!atomic_load_explicit(&profiler_running, memory_order_acquire))
		return;

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

/* Sampling is timer-driven, so the pump-side sample_tick is a no-op —
 * matches the FreeRTOS contract. */
void ove_backend_profiler_sample_tick(void)
{
}

static void profiler_timer_expiry(struct k_timer *timer)
{
	(void)timer;
	ove_backend_profiler_on_tick();
}

static k_timeout_t period_for_hz(uint32_t hz)
{
	if (hz == 0)
		hz = 1;
	/* K_USEC gives sub-millisecond periods when the tick rate allows;
	 * Zephyr clamps to the nearest tick internally. */
	uint32_t us = 1000000u / hz;
	if (us == 0)
		us = 1;
	return K_USEC(us);
}

int ove_backend_profiler_start(void)
{
	/* Idempotent: re-start with the current rate. */
	k_timer_init(&profiler_timer, profiler_timer_expiry, NULL);
	k_timeout_t period = period_for_hz(CONFIG_OVE_PROFILER_HZ);
	k_timer_start(&profiler_timer, period, period);
	atomic_store_explicit(&profiler_running, 1, memory_order_release);
	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
	atomic_store_explicit(&profiler_running, 0, memory_order_release);
	k_timer_stop(&profiler_timer);
}

void ove_backend_profiler_set_rate(uint32_t hz)
{
	uint32_t max_hz = ove_backend_profiler_get_max_hz();
	if (hz == 0 || hz > max_hz)
		hz = max_hz;

	k_timeout_t period = period_for_hz(hz);
	/* k_timer_start on a running timer restarts it atomically. */
	k_timer_start(&profiler_timer, period, period);
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	/* Capped by the system clock granularity. At 1 kHz tick rate
	 * anything above 1 kHz collapses onto the tick anyway. */
	uint32_t cfg_hz  = (uint32_t)CONFIG_OVE_PROFILER_HZ;
	uint32_t tick_hz = (uint32_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	return (cfg_hz < tick_hz) ? cfg_hz : tick_hz;
}

/*
 * Zephyr symbolicates host-side: the dashboard bridge runs
 * arm-none-eabi-nm on the linked ELF, same as FreeRTOS.
 */
size_t ove_backend_profiler_drain_symbols(char *out, size_t out_max)
{
	(void)out;
	(void)out_max;
	return 0;
}

#endif /* CONFIG_OVE_PROFILER */
