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
 * Multi-frame unwinding via stack scanning:
 *   A classic r7 FP-chain walk doesn't work here — GCC's
 *   -fno-omit-frame-pointer on Thumb-2 emits prologues like
 *     push {r7, lr}
 *     sub  sp, #N          ; N = locals size
 *     add  r7, sp, #0      ; r7 = SP after locals
 *   so r7 points at the start of the locals area, not at the saved r7
 *   slot, and the offset from r7 to the saved LR varies per function
 *   (N + 4). `[r7+4]` reads a local, not the return address.
 *
 *   Instead we scan the interrupted task's stack above the 8-word
 *   hardware exception frame, looking for words that look like a
 *   saved LR — Thumb bit set, cleared value points into .text
 *   (bounded by linker-defined __etext). FreeRTOS pre-fills unused
 *   stack with 0xA5A5A5A5 (tskSTACK_FILL_BYTE), so hitting that value
 *   terminates the scan without needing to query the TCB for the
 *   stack high-water mark. False positives are rare (~1/16 k random
 *   words pass the mask+range filter).
 *
 *   The hardware-stacked LR from the exception frame is NOT used as a
 *   normal caller frame — in any function that has already executed an
 *   internal `bl` and returned, LR holds a stale in-function address
 *   and treating it as the direct caller injects a phantom self-edge
 *   below the leaf in the flame graph. It's only used as a fallback
 *   when the scan finds no saved {r7, lr} pair (true-leaf functions or
 *   prologue-interrupted samples).
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

#include "ove/profiler.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/types.h"
#include "ove_profiler_ring.h"

#ifndef CONFIG_OVE_PROFILER_HZ
#define CONFIG_OVE_PROFILER_HZ 250
#endif

/* Cortex-M basic exception frame layout (word indices from stack pointer). */
enum {
	EXC_FRAME_R0   = 0,
	EXC_FRAME_R1   = 1,
	EXC_FRAME_R2   = 2,
	EXC_FRAME_R3   = 3,
	EXC_FRAME_R12  = 4,
	EXC_FRAME_LR   = 5,
	EXC_FRAME_PC   = 6,
	EXC_FRAME_XPSR = 7,
};

/* SRAM bounds for FP-chain walking. MPS2-AN500 linker script puts RAM at
 * 0x20000000 + 4 MB (boards/qemu-mps2-an500/freertos/mps2_an500.ld). Any
 * fp outside this range can't point at a live stack frame — stop walking. */
#define OVE_SRAM_BASE 0x20000000u
#define OVE_SRAM_END  0x20400000u

/* Text section bounds: [__stext, __etext). __stext is placed right after
 * the ISR vector table in the linker script — using FLASH origin (0x0)
 * would let small integers that happen to have the Thumb bit set pass
 * the is-text-addr filter and get symbolicated as fake offsets into
 * __isr_vector (e.g., "__isr_vector+0x2"). Upper bound is the linker-
 * defined __etext at the end of .text. A walked saved-LR outside this
 * range can't be a real return address. */
extern uint32_t __stext;
extern uint32_t __etext;
#define OVE_TEXT_BASE ((uintptr_t)&__stext)
#define OVE_TEXT_END  ((uintptr_t)&__etext)

/* FreeRTOS task stack fill pattern (tskSTACK_FILL_BYTE = 0xA5 repeated).
 * Hitting this during the scan means we've reached unused stack. */
#define OVE_STACK_FILL 0xA5A5A5A5u

/* Hard cap on how many stack words we'll inspect per sample. 256 words =
 * 1 KiB — larger than any typical in-progress call chain, small enough
 * that an ISR-context linear scan stays cheap and can't run off the end
 * of a small task's stack into adjacent memory (the fill-pattern check
 * normally catches it first, but this is a belt-and-braces bound). */
#define OVE_STACK_SCAN_WORDS 256u

/* Return true iff `lr_clean` points right after a `bl imm` (32-bit) or
 * `blx reg` (16-bit) Thumb-2 instruction — the only Thumb encodings that
 * set LR with return-address semantics. lr_clean is a Thumb-mode return
 * address (bit 0 already stripped) validated to sit inside .text.
 *
 * Encodings (ARMv7-M Thumb-2):
 *   bl  imm  (T1, 32-bit):
 *       hw1 @ (lr-4): 11110 S ...          -> (hw1 & 0xF800) == 0xF000
 *       hw2 @ (lr-2): 11 J1 1 J2 ...       -> (hw2 & 0xD000) == 0xD000
 *                                             (bits 15, 14, 12 all set)
 *   blx reg (T1, 16-bit):
 *       hw2 @ (lr-2): 0100 0111 1 Rm 000   -> (hw2 & 0xFF87) == 0x4780
 *
 * Reads hit .text (flash) so they're safe from ISR context and cheap. */
static inline int ove_lr_is_post_bl(uintptr_t lr_clean)
{
	if (lr_clean < OVE_TEXT_BASE + 4 || lr_clean >= OVE_TEXT_END)
		return 0;
	if (lr_clean & 1u)
		return 0;  /* can't happen: caller cleared the Thumb bit */
	uint16_t hw2 = *(const uint16_t *)(lr_clean - 2);
	if ((hw2 & 0xFF87u) == 0x4780u)
		return 1;  /* blx reg */
	uint16_t hw1 = *(const uint16_t *)(lr_clean - 4);
	if ((hw1 & 0xF800u) == 0xF000u && (hw2 & 0xD000u) == 0xD000u)
		return 1;  /* bl imm */
	return 0;
}

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
	uint32_t pc = frame[EXC_FRAME_PC] & ~1u;

	struct ove_profiler_sample s;
	memset(&s, 0, sizeof(s));
	s.ts_us = ove_state_stats_now_us();
	s.tid   = (uint32_t)tid;
	s.state = OVE_THREAD_STATE_RUNNING;
	s.pcs[0] = (uintptr_t)pc;
	s.depth  = 1;
	size_t i = 1;

	/*
	 * Frames 1+: scan the task's stack above the hardware exception
	 * frame for words that look like saved return addresses.
	 *
	 * Filter per word: two adjacent words must look like a saved
	 * {r7, lr} pair pushed by a standard Thumb prologue:
	 *   - curr word: Thumb bit set + cleared value in [__stext, __etext)
	 *   - prev word: SRAM-resident address strictly above the curr
	 *     word's position (i.e., points at an outer frame's r7 slot —
	 *     r7 always points above where it was stacked, because it was
	 *     set from SP AFTER locals were allocated below it).
	 * The prev-word check prunes the dominant false-positive class:
	 * stale saved-LR values left behind when an earlier bl returned
	 * and the caller moved on to a new bl. Those values are still real
	 * code addresses and pass the text-range filter, but they aren't
	 * paired with a saved-r7 slot in the current live frame chain, so
	 * the adjacent-word heuristic rejects them.
	 *
	 * Chain-consistency filter (last_r7): r7 values of live frames form
	 * a strictly ascending sequence moving outward — each outer frame's
	 * r7 sits at a higher address than the inner frame's r7. Once one
	 * pair has been accepted, every subsequent candidate is required to
	 * have BOTH its saved_r7 value AND its own position strictly greater
	 * than the previously accepted saved_r7. This kills the second major
	 * class of false positives: uninitialised / residual words inside an
	 * outer frame's locals that look like a {saved_r7, saved_lr} pair
	 * but carry a saved_r7 value pointing at or below a frame we've
	 * already walked past. In practice those residuals are copies of
	 * handler-r7 left behind by previously-returned callees (e.g.
	 * ove_mutex_lock before lv_timer_handler), and they surface in the
	 * flame graph as phantom frames sandwiched between legit callers.
	 *
	 * BL-precedes-LR filter: every real saved_lr points to the
	 * instruction right after a `bl` or `blx` — the only Thumb-2
	 * instructions that set LR with an actual return-address semantics.
	 * Function pointers stored as data (LVGL draw-task callbacks, vtable
	 * entries, FreeRTOS timer fns) point to function ENTRY addresses,
	 * whose preceding bytes are whatever was in the text section before
	 * that function (padding, literal pool, prior function's tail) and
	 * almost never match the bl/blx encoding. Reading the two halfwords
	 * at (lr-4) and (lr-2) and verifying the encoding eliminates the
	 * third major phantom class — stray function-pointer values that
	 * happen to sit adjacent to an SRAM-looking word in an outer frame's
	 * locals (xTaskCheckForTimeOut, event_send_core, block_next, etc.).
	 *
	 * Terminate the scan on:
	 *   - hitting 0xA5A5A5A5 fill pattern (unused stack above SP)
	 *   - leaving the SRAM region
	 *   - walking past OVE_STACK_SCAN_WORDS words
	 *   - filling MAX_DEPTH slots
	 */
	const uint32_t *scan = frame + 8;  /* past exc frame (8 words) */
	const uint32_t *scan_end = scan + OVE_STACK_SCAN_WORDS;
	uint32_t prev_word = 0;
	uintptr_t last_r7 = 0;  /* most recently accepted saved_r7 value */
	while (i < CONFIG_OVE_PROFILER_MAX_DEPTH && scan < scan_end) {
		uintptr_t sp_addr = (uintptr_t)scan;
		if (sp_addr < OVE_SRAM_BASE || sp_addr >= OVE_SRAM_END)
			break;
		uint32_t val = *scan;
		if (val == OVE_STACK_FILL)
			break;
		scan++;
		if ((val & 1u) == 0) {
			prev_word = val;
			continue;
		}
		uint32_t lr_clean = val & ~1u;
		if (lr_clean < OVE_TEXT_BASE || lr_clean >= OVE_TEXT_END) {
			prev_word = val;
			continue;
		}
		/* BL-precedes-LR: drop values that don't look like return
		 * addresses (e.g. function pointers stored as data). */
		if (!ove_lr_is_post_bl(lr_clean)) {
			prev_word = val;
			continue;
		}
		/* Saved-r7 invariant. Standard Thumb-2 prologues
		 *   push {r7, lr}         (or push {r4, r7, lr})
		 *   sub sp, #N
		 *   add r7, sp, #K
		 * store a saved_r7 VALUE = (caller SP at call) + K_caller,
		 * where K_caller is the caller's own `add r7, sp, #K` offset.
		 * Caller SP at call = saved_lr_slot_addr + 4, so saved_r7 value
		 * = saved_lr_slot_addr + 4 + K_caller. Histogram of K values
		 * in this build peaks at K=0 (94 % of prologues) with a long
		 * tail up to K=108; allow [0, 128] for headroom. Anything
		 * outside that is random data / function pointers / unrelated
		 * addresses, rejected. Within the range, stale saved-{r7, lr}
		 * pairs from previously-completed deep calls (e.g. stale
		 * block_is_last / event_is_trickled saves in the locals of
		 * lv_draw_sw_fill) are indistinguishable from live ones
		 * without DWARF CFI — they surface as residual phantoms and
		 * are a documented limitation of the frame-pointer walker. */
		uint32_t r7_delta = prev_word - (uint32_t)(sp_addr + 4);
		if (r7_delta > 128u) {
			prev_word = val;
			continue;
		}
		/* Chain constraint (only once a first frame has been locked in):
		 * outer frames' r7s are strictly ascending, so both the new
		 * saved_r7 and the new pair's own position must sit above
		 * last_r7 (which is the outer-frame locals-top we just walked
		 * past). sp_addr is at the saved_lr slot, whose companion
		 * saved_r7 is at sp_addr-4, so the pair's footprint starts at
		 * sp_addr-4; that must be strictly > last_r7. */
		if (last_r7 != 0 &&
		    (prev_word <= last_r7 || (sp_addr - 4) <= last_r7)) {
			prev_word = val;
			continue;
		}
		s.pcs[i++] = (uintptr_t)lr_clean;
		s.depth = (uint8_t)i;
		last_r7 = prev_word;
		prev_word = val;
	}

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
		uint32_t stacked_lr = frame[EXC_FRAME_LR] & ~1u;
		if (stacked_lr != 0 && stacked_lr != pc &&
		    stacked_lr >= OVE_TEXT_BASE && stacked_lr < OVE_TEXT_END &&
		    ove_lr_is_post_bl((uintptr_t)stacked_lr)) {
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
