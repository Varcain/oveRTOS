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
	OVE_ARM_EXC_R0 = 0,
	OVE_ARM_EXC_R1 = 1,
	OVE_ARM_EXC_R2 = 2,
	OVE_ARM_EXC_R3 = 3,
	OVE_ARM_EXC_R12 = 4,
	OVE_ARM_EXC_LR = 5,
	OVE_ARM_EXC_PC = 6,
	OVE_ARM_EXC_XPSR = 7,
};

/**
 * @file ove_arm_backtrace.h
 * @brief Validate that an LR points right after a Thumb-2 `bl imm`
 *        (32-bit) or `blx reg` (16-bit) — the only Thumb encodings
 *        that set LR with return-address semantics.
 *
 * Used by the walker to reject stray function-pointer values that pass
 * the text-range filter but aren't real return addresses (e.g. LVGL
 * draw-task callbacks, vtable entries, timer callbacks).
 *
 * Safe to call from ISR context — reads hit .text (flash), no SRAM access.
 *
 * @param[in] lr_clean  Candidate LR with Thumb bit already cleared.
 * @param[in] text_lo   Inclusive lower .text bound.
 * @param[in] text_hi   Exclusive upper .text bound.
 * @return Non-zero when the two halfwords at (lr-4) / (lr-2) encode
 *         `bl imm` or `blx reg`, zero otherwise.
 */
int ove_arm_backtrace_lr_is_post_bl(uintptr_t lr_clean, uintptr_t text_lo, uintptr_t text_hi);

/**
 * @brief Locate the exception frame pushed when an ISR interrupted a
 *        handler (Cortex-M, MSP stack).
 *
 * When the sampling interrupt fires while the CPU is already in handler
 * mode, the hardware stacks the interrupted handler's r0-r3, r12, lr, pc
 * and xpsr on the MSP. By the time the sampler's C code runs, its own
 * prologue has pushed registers and locals on top, so a plain
 * `__get_MSP()` does not point at that frame, and the sampler's locals in
 * between can look like one. This helper scans upward from @p msp for the
 * 8-word block right above the sampler's saved EXC_RETURN (0xFFFFFFF1 or
 * 0xFFFFFFE1: return to a handler), which the sampling handler's first
 * push stores directly below the hardware frame. The block's xpsr must
 * have the Thumb bit set, its reserved bits [23:20] clear and an
 * exception number a preempted handler can have, other than @p own_exc,
 * and its pc must lie in [@p text_lo, @p text_hi). The lowest such block
 * wins: words the sampler has not written yet can still hold an older,
 * anchored frame, so a stale handler frame further down can be reported
 * instead of the real one (rare: it takes a nested handler that left one
 * deeper than the sampler's entry). The anchors are the ARMv7-M and
 * Armv8-M Secure EXC_RETURN values; an Armv8-M Non-secure build would
 * need its own.
 *
 * Only call this when the sampler is nested on another handler
 * (ICSR.RETTOBASE == 0); otherwise the interrupted frame is on the PSP.
 *
 * @param[in] msp        Current MSP value.
 * @param[in] msp_limit  Exclusive upper bound of the MSP stack (e.g. the
 *                       linker's stack top). Never read at or above it.
 * @param[in] text_lo    Inclusive .text lower bound.
 * @param[in] text_hi    Exclusive .text upper bound.
 * @param[in] max_words  Maximum number of words to scan (0 = no cap
 *                       beyond @p msp_limit).
 * @param[in] own_exc    The sampling handler's own exception number,
 *                       never reported as the interrupted one.
 * @return Pointer to the frame's r0 slot, or NULL when no plausible
 *         handler-mode frame was found.
 */
const uint32_t *ove_arm_find_exc_frame(uintptr_t msp, uintptr_t msp_limit, uintptr_t text_lo,
				       uintptr_t text_hi, unsigned int max_words,
				       unsigned int own_exc);

/**
 * @brief Multi-frame stack-scan unwinder for Cortex-M / Thumb-2 tasks.
 *
 * A classic r7 FP-chain walk doesn't work on GCC with
 * -fno-omit-frame-pointer — the compiler emits
 * @code
 *   push {r7, lr}
 *   sub  sp, #N
 *   add  r7, sp, #K     ; K typically 0, occasionally up to ~108
 * @endcode
 * so r7 points at the start of locals, not at the saved-r7 slot, and
 * the offset from r7 to the saved-LR slot varies per function.
 *
 * Instead the walker scans words above the 8-word hardware exception
 * frame, looking for pairs that look like a saved {r7, lr} push. A
 * candidate word must:
 *   - have the Thumb bit set,
 *   - point (after clearing bit 0) into [@p text_lo, @p text_hi),
 *   - sit right after a `bl imm` or `blx reg`,
 *   - have a plausible saved-r7 word in the push block below the saved
 *     LR (at most five words: the callee may save r8-r11 between the
 *     saved r7 and the saved LR): saved_r7_value =
 *     (saved_lr_slot_addr + 4) + K_caller for K_caller in [0, 128].
 *     When several words of the block qualify, the one whose value is
 *     closest to (saved_lr_slot_addr + 4) — the modal K_caller = 0 — is
 *     taken as the saved r7 (when the callee saves high registers, the
 *     word nearest the saved LR is a saved r8/r9/sl/fp, not the saved
 *     r7),
 *   - the pair's footprint must sit strictly above the most recently
 *     accepted saved-r7 (ascending-chain invariant).
 *
 * A stale return address left in a frame's locals can still pass all of
 * these, so the result may contain phantom frames; the dashboard bridge
 * prunes them against the control-flow graph.
 *
 * Scan terminates at the top of the task's stack (@p stack_hi), after a
 * 16 KiB safety cap, or on filling @p max slots. Returns 0 when @p psp is
 * outside [@p stack_lo, @p stack_hi).
 *
 * @param[in]  psp      PSP at exception entry (or equivalent — points at the
 *                      stacked R0 of the interrupted task). Caller is
 *                      responsible for pcs[0]=stacked PC; this function
 *                      writes frames 1..max-1. frame[OVE_ARM_EXC_LR] is
 *                      NOT used as a normal frame — caller may use it as
 *                      a fallback only when this function returns 0.
 * @param[in]  text_lo  Inclusive .text lower bound (must exclude ISR vector
 *                      table so small integer stack values can't alias vectors).
 * @param[in]  text_hi  Exclusive .text upper bound (linker @c __etext /
 *                      @c __text_region_end).
 * @param[in]  stack_lo Inclusive lower bound of the interrupted task's stack.
 * @param[in]  stack_hi Exclusive upper bound (top) of the task's stack. A
 *                      fill pattern cannot serve as the bound: never-written
 *                      locals in live frames still hold it.
 * @param[out] out      Output buffer for walked return addresses, Thumb
 *                      bit cleared, inner frames first.
 * @param[in]  max      Capacity of @p out (number of slots).
 * @return Number of slots written to @p out (0..max).
 */
int ove_arm_backtrace_walk(uintptr_t psp, uintptr_t text_lo, uintptr_t text_hi, uintptr_t stack_lo,
			   uintptr_t stack_hi, uintptr_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* OVE_ARM_BACKTRACE_H */
