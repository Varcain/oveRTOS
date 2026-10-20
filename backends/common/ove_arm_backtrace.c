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
 * RTOS-specific pieces are the .text range and the interrupted task's
 * stack range — both passed as parameters.
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
 * 3. Saved-r7 presence, and the candidate picked BY VALUE: somewhere in
 *    the push block just below the saved-LR slot there must be a
 *    plausible saved-r7 value — an SRAM address equal to
 *    (saved_lr_slot_addr + 4) + K_caller for K_caller in [0, 128].  It
 *    is not always the immediately-preceding word: GCC emits
 *    `stmdb sp!, {r4-r9, sl, fp, lr}` for functions that use high
 *    registers, leaving r8/r9/sl/fp between the saved r7 and the saved
 *    LR.  When more than one word of the block qualifies, the one whose
 *    VALUE is closest to (saved_lr_slot_addr + 4) is the saved r7 —
 *    that is the modal K = 0 of the histogram below.  The word nearest
 *    the saved LR is a saved r8/r9/sl/fp, not a saved r7, whenever the
 *    callee saves high registers; such a word passes the value filter
 *    whenever the callee-saved register happens to hold a pointer into
 *    an outer frame.  Rejects stale LR values left on the stack from
 *    earlier completed calls (they pass the text-range and
 *    bl-precedes filters but no word in the push block is a fitting
 *    SRAM pointer).
 * 4. Chain-ascending invariant: once a first pair is accepted, the
 *    word just below each later saved LR, and the saved-r7 value picked
 *    for it, must sit strictly above the most recently accepted
 *    saved-r7. Rejects residual {r7,lr} pairs in an outer frame's
 *    locals that look valid in isolation but sit at or below the address
 *    the accepted saved-r7 points to (the part of that frame below its
 *    r7); a stale pair above that address in the same frame still gets
 *    through.  The position half is checked at the word below the saved
 *    LR, not at the saved-r7 slot the value filter picks, which can be up
 *    to five words lower.
 * 5. Scan termination: stops at the top of the task's stack, after
 *    OVE_ARM_STACK_SCAN_MAX_WORDS, or on filling all @max slots. The
 *    scan is bounded by the real stack range rather than by a fill
 *    pattern: never-written locals inside live frames still hold the
 *    fill pattern (which cut chains short), and nothing else stopped
 *    the scan at the stack top, past which it reported frames from
 *    neighbouring memory.
 *
 * Even with all five filters, stale saved-{r7, lr} pairs from
 * previously-completed deep calls whose addresses fall inside the
 * current frame's locals remain indistinguishable from live frames
 * without DWARF CFI. The dashboard bridge runs a CFG-based pruner over
 * the resolved chain to drop those residual phantoms.
 */

#include <stdint.h>

#include "ove/ove_arm_backtrace.h"

/* Safety cap on how many stack words to inspect per sample (16 KiB).
 * The stack range normally ends the scan first; this bounds the ISR-side
 * cost for very large stacks. Deep LVGL call chains need several KiB. */
#define OVE_ARM_STACK_SCAN_MAX_WORDS 4096u

int ove_arm_backtrace_lr_is_post_bl(uintptr_t lr_clean, uintptr_t text_lo, uintptr_t text_hi)
{
	if (lr_clean < text_lo + 4 || lr_clean >= text_hi)
		return 0;
	if (lr_clean & 1u)
		return 0; /* caller is expected to clear the Thumb bit */

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
		return 1; /* blx reg */
	uint16_t hw1 = *(const uint16_t *)(lr_clean - 4);
	if ((hw1 & 0xF800u) == 0xF000u && (hw2 & 0xD000u) == 0xD000u)
		return 1; /* bl imm */
	return 0;
}

int ove_arm_backtrace_walk(uintptr_t psp, uintptr_t text_lo, uintptr_t text_hi, uintptr_t stack_lo,
			   uintptr_t stack_hi, uintptr_t *out, int max)
{
	if (!out || max <= 0)
		return 0;
	/* PSP outside the task's stack: overflowed, or not this task's
	 * stack (e.g. a privileged stack). Nothing trustworthy to scan. */
	if (psp < stack_lo || psp >= stack_hi)
		return 0;

	/* Skip the 8-word hardware exception frame at PSP. */
	const uint32_t *scan = (const uint32_t *)(psp) + 8;
	const uint32_t *scan_end = (const uint32_t *)(stack_hi & ~(uintptr_t)3u);
	if (scan >= scan_end)
		return 0;
	if ((uintptr_t)scan_end - (uintptr_t)scan > OVE_ARM_STACK_SCAN_MAX_WORDS * 4u)
		scan_end = scan + OVE_ARM_STACK_SCAN_MAX_WORDS;

	uintptr_t last_r7 = 0; /* most recently accepted saved_r7 value */
	int depth = 0;
	const uint32_t *scan_start = scan;

	while (depth < max && scan < scan_end) {
		uintptr_t sp_addr = (uintptr_t)scan;
		uint32_t val = *scan;
		scan++;
		if ((val & 1u) == 0)
			continue;
		uint32_t lr_clean = val & ~1u;
		if (lr_clean < text_lo || lr_clean >= text_hi)
			continue;
		if (!ove_arm_backtrace_lr_is_post_bl(lr_clean, text_lo, text_hi))
			continue;
		/*
		 * Saved-r7 invariant: a function that calls out pushes its
		 * caller's r7 and the return address, then builds its own frame:
		 *   stmdb sp!, {r4-r9, sl, fp, lr}
		 *   sub sp, #N
		 *   add r7, sp, #K
		 * so saved_r7_VALUE = (caller_SP_at_call) + K_caller
		 *                   = (saved_lr_slot_addr + 4) + K_caller.
		 * The saved r7 is not necessarily the word just below the saved
		 * LR: when the callee uses r8-r11, GCC pushes them between r7
		 * and lr.  Search the whole possible push block instead (at most
		 * five words below it: fp, sl, r9 and r8, then r7).
		 * Histogram of K values in example firmware peaks at K=0 (94% of
		 * prologues) with a long tail to K=108; allow [0, 128] headroom.
		 *
		 * The candidate is picked BY VALUE, not by position: among the
		 * words of the push block that pass the filters, the saved r7 is
		 * the one whose value is closest to (saved_lr_slot_addr + 4) —
		 * the modal K = 0 of that histogram.  The word nearest the saved
		 * LR is a saved r8/r9/sl/fp whenever the callee saves high
		 * registers, and such a word passes the value filter whenever
		 * the callee-saved register happens to hold a pointer into an
		 * outer frame.  Anchoring the frame on it mislabels the frame
		 * and breaks the chain-ascending filter for the next one (the
		 * bogus anchor sits above the next frame's real saved r7), so
		 * the caller frame drops out of the trace.
		 */
		const uint32_t *lr_slot = scan - 1;
		const uint32_t *r7_slot = 0;
		uint32_t saved_r7 = 0;
		uint32_t best_delta = 0;
		for (int back = 1; back <= 5; back++) {
			const uint32_t *cand = lr_slot - back;
			if (cand < scan_start)
				break;
			if (*cand - (uint32_t)(sp_addr + 4) > 128u)
				continue;
			/*
			 * Chain-ascending: every outer frame's r7 sits above the
			 * inner frame's r7. Once one pair has been locked in, reject
			 * any candidate whose saved-r7 OR footprint sits at/below the
			 * last accepted saved-r7 — those are residual pairs from
			 * previously-returned callees still sitting in the outer
			 * frame's locals.  The footprint half does not depend on the
			 * candidate, and the value window above already implies the
			 * value half once the footprint half holds.
			 */
			if (last_r7 != 0 && (*cand <= last_r7 || (sp_addr - 4) <= last_r7))
				continue;
			/*
			 * By-value selection: the saved r7 is the word whose value
			 * is closest to (saved_lr_slot_addr + 4).  A tie means two
			 * words hold the same address, so the anchor VALUE — the
			 * only thing the chain rule above sees — is the same
			 * either way; keep the first one found (the one nearest the
			 * saved LR) so the choice stays deterministic.
			 */
			uint32_t delta = *cand - (uint32_t)(sp_addr + 4);
			if (!r7_slot || delta < best_delta) {
				r7_slot = cand;
				saved_r7 = *cand;
				best_delta = delta;
			}
		}
		if (!r7_slot)
			continue;
		out[depth++] = (uintptr_t)lr_clean;
		last_r7 = saved_r7;
	}
	return depth;
}
