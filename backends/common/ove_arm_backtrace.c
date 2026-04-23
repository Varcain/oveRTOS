/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Cortex-M / Thumb-2 multi-frame stack-scan unwinder.
 *
 * Hoisted from backends/freertos/freertos_profiler.c so Zephyr (and any
 * future Cortex-M RTOS backend) can reuse the same heuristic chain
 * without carrying its own copy of the five filters. The only
 * RTOS-specific pieces are the address ranges and the stack-fill
 * pattern — all passed as parameters.
 *
 * A classic r7 FP-chain walk does NOT work here: GCC's
 * -fno-omit-frame-pointer on Thumb-2 emits
 *   push {r7, lr}
 *   sub  sp, #N            ; N = locals size
 *   add  r7, sp, #K        ; K typically 0, occasionally up to ~108
 * so r7 points at the start of locals, not at the saved-r7 slot, and
 * the offset from r7 to the saved-LR slot varies per function. A plain
 * `[r7+4]` read returns a local variable, not a return address.
 *
 * Instead we scan words above the 8-word hardware exception frame for
 * values that could be a saved-LR pushed by a standard Thumb-2 prologue,
 * and apply the following filters in order:
 *
 * 1. Thumb bit set + cleared value in [text_lo, text_hi)  — must
 *    look like a .text address in Thumb-mode encoding.
 * 2. The two halfwords at (lr-4) and (lr-2) encode `bl imm` (T1, 32-bit)
 *    or `blx reg` (T1, 16-bit) — the only Thumb-2 instructions that
 *    set LR with return-address semantics. Rejects stray function-
 *    pointer values stored as data (vtable entries, timer callbacks,
 *    LVGL draw-task callbacks).
 * 3. Saved-r7 adjacency: the immediately-preceding word must be a
 *    plausible saved-r7 value — an SRAM address equal to
 *    (saved_lr_slot_addr + 4) + K_caller for K_caller in [0, 128].
 *    Rejects stale LR values left on the stack from earlier completed
 *    calls (they pass the text-range and bl-precedes filters but their
 *    adjacent word isn't a fitting SRAM pointer).
 * 4. Chain-ascending invariant: once a first pair is accepted, every
 *    subsequent candidate's saved-r7 value AND its own position must
 *    sit strictly above the most recently accepted saved-r7. Rejects
 *    residual {r7,lr} pairs in an outer frame's locals that look valid
 *    in isolation but carry a saved-r7 pointing into frames we've
 *    already walked past.
 * 5. Scan termination: stops on the fill pattern (when enabled),
 *    on leaving SRAM, on hitting OVE_ARM_STACK_SCAN_WORDS, or on
 *    filling all @max slots.
 *
 * Even with all five filters, stale saved-{r7, lr} pairs from
 * previously-completed deep calls whose addresses fall inside the
 * current frame's locals remain indistinguishable from live frames
 * without DWARF CFI. The dashboard bridge runs a CFG-based pruner over
 * the resolved chain to drop those residual phantoms; see gotcha 26b
 * in memory/project_profiler_trace.md.
 */

#include <stdint.h>

#include "ove/ove_arm_backtrace.h"

/* Hard cap on how many stack words to inspect per sample. 256 words =
 * 1 KiB — larger than any realistic in-progress call chain, small
 * enough that a linear scan stays cheap from ISR context and bounds
 * the cost if the SRAM/fill/filter checks all miss. */
#define OVE_ARM_STACK_SCAN_WORDS 256u

int ove_arm_backtrace_lr_is_post_bl(uintptr_t lr_clean,
				    uintptr_t text_lo, uintptr_t text_hi)
{
	if (lr_clean < text_lo + 4 || lr_clean >= text_hi)
		return 0;
	if (lr_clean & 1u)
		return 0;  /* caller is expected to clear the Thumb bit */

	/*
	 * Thumb-2 encodings (ARMv7-M):
	 *   bl imm  (T1, 32-bit):
	 *     hw1 @ (lr-4): 11110 S ...        -> (hw1 & 0xF800) == 0xF000
	 *     hw2 @ (lr-2): 11 J1 1 J2 ...     -> (hw2 & 0xD000) == 0xD000
	 *                                         (bits 15, 14, 12 all set)
	 *   blx reg (T1, 16-bit):
	 *     hw2 @ (lr-2): 0100 0111 1 Rm 000 -> (hw2 & 0xFF87) == 0x4780
	 *
	 * Reads hit .text (flash/ROM) so they're safe from ISR context.
	 */
	uint16_t hw2 = *(const uint16_t *)(lr_clean - 2);
	if ((hw2 & 0xFF87u) == 0x4780u)
		return 1;  /* blx reg */
	uint16_t hw1 = *(const uint16_t *)(lr_clean - 4);
	if ((hw1 & 0xF800u) == 0xF000u && (hw2 & 0xD000u) == 0xD000u)
		return 1;  /* bl imm */
	return 0;
}

int ove_arm_backtrace_walk(uintptr_t psp,
			   uintptr_t text_lo, uintptr_t text_hi,
			   uintptr_t sram_lo, uintptr_t sram_hi,
			   uint32_t fill,
			   uintptr_t *out, int max)
{
	if (!out || max <= 0)
		return 0;

	/* Skip the 8-word hardware exception frame at PSP. */
	const uint32_t *scan = (const uint32_t *)(psp) + 8;
	const uint32_t *scan_end = scan + OVE_ARM_STACK_SCAN_WORDS;

	uint32_t prev_word = 0;
	uintptr_t last_r7 = 0;  /* most recently accepted saved_r7 value */
	int depth = 0;

	while (depth < max && scan < scan_end) {
		uintptr_t sp_addr = (uintptr_t)scan;
		if (sp_addr < sram_lo || sp_addr >= sram_hi)
			break;
		uint32_t val = *scan;
		if (fill != 0 && val == fill)
			break;
		scan++;
		if ((val & 1u) == 0) {
			prev_word = val;
			continue;
		}
		uint32_t lr_clean = val & ~1u;
		if (lr_clean < text_lo || lr_clean >= text_hi) {
			prev_word = val;
			continue;
		}
		if (!ove_arm_backtrace_lr_is_post_bl(lr_clean, text_lo, text_hi)) {
			prev_word = val;
			continue;
		}
		/*
		 * Saved-r7 invariant: prologue
		 *   push {r7, lr}
		 *   sub sp, #N
		 *   add r7, sp, #K
		 * stores saved_r7_VALUE = (caller_SP_at_call) + K_caller
		 *                       = (saved_lr_slot_addr + 4) + K_caller.
		 * Histogram of K values in example firmware peaks at K=0
		 * (94% of prologues) with a long tail to K=108; allow [0, 128]
		 * for headroom.
		 */
		uint32_t r7_delta = prev_word - (uint32_t)(sp_addr + 4);
		if (r7_delta > 128u) {
			prev_word = val;
			continue;
		}
		/*
		 * Chain-ascending: every outer frame's r7 sits above the inner
		 * frame's r7. Once one pair has been locked in, reject any
		 * candidate whose saved-r7 OR footprint sits at/below the last
		 * accepted saved-r7 — those are residual pairs from previously-
		 * returned callees still sitting in the outer frame's locals.
		 */
		if (last_r7 != 0 &&
		    (prev_word <= last_r7 || (sp_addr - 4) <= last_r7)) {
			prev_word = val;
			continue;
		}
		out[depth++] = (uintptr_t)lr_clean;
		last_r7 = prev_word;
		prev_word = val;
	}
	return depth;
}
