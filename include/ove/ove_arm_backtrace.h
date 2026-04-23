/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_ARM_BACKTRACE_H
#define OVE_ARM_BACKTRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cortex-M basic exception frame layout (word indices from PSP at
 * exception entry). Declared here so profiler backends can read the
 * stacked PC/LR directly without redefining the enum.
 */
enum {
	OVE_ARM_EXC_R0   = 0,
	OVE_ARM_EXC_R1   = 1,
	OVE_ARM_EXC_R2   = 2,
	OVE_ARM_EXC_R3   = 3,
	OVE_ARM_EXC_R12  = 4,
	OVE_ARM_EXC_LR   = 5,
	OVE_ARM_EXC_PC   = 6,
	OVE_ARM_EXC_XPSR = 7,
};

/**
 * ove_arm_backtrace_lr_is_post_bl — validate that @lr_clean points
 * right after a Thumb-2 `bl imm` (32-bit) or `blx reg` (16-bit) — the
 * only Thumb encodings that set LR with return-address semantics.
 *
 * Used by the walker to reject stray function-pointer values that pass
 * the text-range filter but aren't real return addresses (e.g. LVGL
 * draw-task callbacks, vtable entries, timer callbacks).
 *
 * @lr_clean: candidate LR with Thumb bit already cleared.
 * @text_lo: inclusive lower .text bound.
 * @text_hi: exclusive upper .text bound.
 *
 * Returns non-zero when the two halfwords at (lr-4) / (lr-2) encode
 * `bl imm` or `blx reg`. Safe to call from ISR context — reads hit
 * .text (flash), no SRAM access.
 */
int ove_arm_backtrace_lr_is_post_bl(uintptr_t lr_clean,
				    uintptr_t text_lo, uintptr_t text_hi);

/**
 * ove_arm_backtrace_walk — multi-frame stack-scan unwinder for
 * Cortex-M / Thumb-2 tasks.
 *
 * A classic r7 FP-chain walk doesn't work on GCC with
 * -fno-omit-frame-pointer — the compiler emits
 *   push {r7, lr}
 *   sub  sp, #N
 *   add  r7, sp, #K     ; K typically 0, occasionally up to ~108
 * so r7 points at the start of locals, not at the saved-r7 slot, and
 * the offset from r7 to the saved-LR slot varies per function.
 *
 * Instead the walker scans words above the 8-word hardware exception
 * frame, looking for pairs that look like a saved {r7, lr} push. A
 * candidate word must:
 *   - have the Thumb bit set,
 *   - point (after clearing bit 0) into [@text_lo, @text_hi),
 *   - sit right after a `bl imm` or `blx reg`,
 *   - have its preceding word (the saved-r7) fit the prologue
 *     invariant: saved_r7_value = (saved_lr_slot_addr + 4) + K_caller
 *     for K_caller in [0, 128],
 *   - the pair's footprint must sit strictly above the most recently
 *     accepted saved-r7 (ascending-chain invariant).
 *
 * Scan terminates on: fill-pattern match (when @fill != 0), leaving
 * SRAM, hitting OVE_ARM_STACK_SCAN_WORDS, or filling @max slots.
 *
 * @psp: PSP at exception entry (or equivalent — points at the
 *       stacked R0 of the interrupted task). Caller is responsible
 *       for pcs[0]=stacked PC; this function writes frames 1..max-1.
 *       frame[OVE_ARM_EXC_LR] is NOT used as a normal frame — caller
 *       may use it as a fallback only when this function returns 0.
 * @text_lo: inclusive .text lower bound (must exclude ISR vector
 *           table so small integer stack values can't alias vectors).
 * @text_hi: exclusive .text upper bound (linker __etext /
 *           __text_region_end).
 * @sram_lo: inclusive SRAM lower bound.
 * @sram_hi: exclusive SRAM upper bound.
 * @fill:    stack-fill pattern that terminates the scan, or 0 to
 *           disable the check. FreeRTOS uses 0xA5A5A5A5 from
 *           tskSTACK_FILL_BYTE; Zephyr without CONFIG_INIT_STACKS
 *           should pass 0.
 * @out:     output buffer for walked return addresses, Thumb bit
 *           cleared, inner frames first.
 * @max:     capacity of @out (number of slots).
 *
 * Returns the number of slots written to @out (0..max).
 */
int ove_arm_backtrace_walk(uintptr_t psp,
			   uintptr_t text_lo, uintptr_t text_hi,
			   uintptr_t sram_lo, uintptr_t sram_hi,
			   uint32_t fill,
			   uintptr_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* OVE_ARM_BACKTRACE_H */
