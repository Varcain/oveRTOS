/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host test for the Cortex-M stack-scan unwinder
 * (backends/common/ove_arm_backtrace.c) on synthetic stacks.
 *
 * The walker treats stack words as 32-bit addresses, so the fake .text and
 * stack regions are mapped below 4 GiB (MAP_32BIT, x86-64 Linux). Where that
 * mapping is unavailable the test is skipped.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "ove/ove_arm_backtrace.h"

/* MAP_32BIT is x86-64 Linux only; elsewhere map normally and skip below
 * when the address does not fit in 32 bits. */
#ifdef MAP_32BIT
#define MAP_LOW MAP_32BIT
#else
#define MAP_LOW 0
#endif

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

#define TEXT_BYTES 0x1000u
#define STACK_WORDS 512u
#define FILL 0xA5A5A5A5u

static uint16_t *text;	/* fake .text, halfwords */
static uint32_t *stack; /* fake task stack */

static uintptr_t text_lo(void)
{
	return (uintptr_t)text;
}

static uintptr_t text_hi(void)
{
	return (uintptr_t)text + TEXT_BYTES;
}

/* Return address (Thumb bit set) right after a `bl` placed at slot k. */
static uint32_t ret_addr(unsigned k)
{
	uintptr_t bl = text_lo() + 0x100u * (k + 1u);
	text[(bl - text_lo()) / 2] = 0xF000;	 /* bl imm, first halfword */
	text[(bl - text_lo()) / 2 + 1] = 0xF800; /* bl imm, second halfword */
	return (uint32_t)(bl + 4u) | 1u;
}

/*
 * Lay out @n frames above an 8-word exception frame at stack[0]. Frame k
 * has @locals words of locals (filled with @local_val) followed by a
 * {saved r7, saved lr} pair, as `push {r7, lr}` stores them. Returns the
 * word index just past the last frame.
 */
static unsigned build_frames(unsigned n, unsigned locals, uint32_t local_val)
{
	memset(stack, 0, STACK_WORDS * 4u);
	unsigned w = 8;
	for (unsigned k = 0; k < n; k++) {
		for (unsigned i = 0; i < locals; i++)
			stack[w++] = local_val;
		unsigned lr_slot = w + 1;
		/* saved r7 = caller SP at the call = lr slot address + 4 (K = 0) */
		stack[w] = (uint32_t)(uintptr_t)&stack[lr_slot + 1];
		stack[lr_slot] = ret_addr(k);
		w = lr_slot + 1;
	}
	return w;
}

static void test_full_chain(void)
{
	unsigned top = build_frames(4, 3, 0x11111111u);
	uintptr_t out[16];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[top], out, 16);
	CHECK(n == 4);
	for (int k = 0; k < n && k < 4; k++)
		CHECK(out[k] == (ret_addr((unsigned)k) & ~1u));
}

/*
 * Functions that use high registers push the whole callee-saved block,
 * e.g. `stmdb sp!, {r4-r9, sl, fp, lr}`, so the saved r7 is up to five
 * words below the saved LR instead of immediately adjacent. The walker
 * used to require adjacency and truncated such chains at the first
 * r8-clobbering frame (Zephyr LVGL stacks never reached thread_wrapper).
 */
static void test_wide_push_block(void)
{
	memset(stack, 0, STACK_WORDS * 4u);
	unsigned w = 8;
	for (unsigned k = 0; k < 3; k++) {
		unsigned lr_slot = w + 8;
		stack[w + 0] = 0x44444444u; /* saved r4 */
		stack[w + 1] = 0x55555555u; /* saved r5 */
		stack[w + 2] = 0x66666666u; /* saved r6 */
		/* saved r7 = caller SP at the call (K = 0) */
		stack[w + 3] = (uint32_t)(uintptr_t)&stack[lr_slot + 1];
		stack[w + 4] = 0x88888888u; /* saved r8 */
		stack[w + 5] = 0x99999999u; /* saved r9 */
		stack[w + 6] = 0xAAAA4444u; /* saved sl */
		stack[w + 7] = 0xBBBB4444u; /* saved fp */
		stack[lr_slot] = ret_addr(k);
		w = lr_slot + 1;
	}
	uintptr_t out[8];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[w], out, 8);
	CHECK(n == 3);
	for (int k = 0; k < n && k < 3; k++)
		CHECK(out[k] == (ret_addr((unsigned)k) & ~1u));
}

/*
 * The saved r7 is not always the word nearest the saved LR: a callee
 * that uses the high registers saves r8-r11 between the saved r7 and
 * the saved LR.  A saved r9 that happens to hold a pointer into an
 * outer frame's locals passes the plausible-saved-r7 value filter, so
 * the candidate has to be picked BY VALUE (the word closest to
 * saved_lr_slot + 4, the modal K = 0) and not by position.  Anchoring
 * on the r9 also breaks the ascending-chain filter for the next frame
 * (the bogus anchor sits above the caller's real saved r7), so the
 * caller drops out of the trace.
 */
static void test_r7_picked_by_value(void)
{
	memset(stack, 0, STACK_WORDS * 4u);

	/*
	 * F0: push {r4, r7, r8, r9, sl, lr}
	 *   words 8..13 hold the saved r4, the saved r7 (K = 0), the saved
	 *   r8, the saved r9, the saved sl and the saved lr.  The saved r9
	 *   holds a pointer to a local of F2: an SRAM address inside the
	 *   plausible-saved-r7 window, but not a saved r7.
	 */
	stack[8] = 0x11111111u;
	stack[9] = (uint32_t)(uintptr_t)&stack[14];
	stack[10] = 0x22222222u;
	stack[11] = (uint32_t)(uintptr_t)&stack[23];
	stack[12] = 0x33333333u;
	stack[13] = ret_addr(0);

	/* F1: two words of locals, then push {r4, r7, r8, r9, sl, lr} */
	stack[14] = 0xEEEEEEEEu;
	stack[15] = 0xEEEEEEEEu;
	stack[16] = 0x44444444u;
	stack[17] = (uint32_t)(uintptr_t)&stack[22];
	stack[18] = 0x55555555u;
	stack[19] = 0x66666666u;
	stack[20] = 0x77777777u;
	stack[21] = ret_addr(1);

	/* F2: three words of locals, then push {r7, lr} */
	stack[22] = 0xEEEEEEEEu;
	stack[23] = 0xEEEEEEEEu;
	stack[24] = 0xEEEEEEEEu;
	stack[25] = (uint32_t)(uintptr_t)&stack[27];
	stack[26] = ret_addr(2);

	uintptr_t out[8];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[27], out, 8);
	CHECK(n == 3);
	for (int k = 0; k < n && k < 3; k++)
		CHECK(out[k] == (ret_addr((unsigned)k) & ~1u));
}

/*
 * The widest prologue GCC emits for a frame-pointer build,
 * `push {r4, r5, r6, r7, r8, r9, sl, fp, lr}`, puts the saved r7 exactly
 * five words below the saved LR — the very edge of the walker's five-word
 * search window.  A callee-saved register sitting next to the saved LR
 * that holds a pointer into an outer frame passes the plausible-saved-r7
 * value filter, so the anchor has to be picked BY VALUE (the word closest
 * to saved_lr_slot + 4) and not by position: anchoring on the high
 * register breaks the ascending-chain filter and drops the caller frames.
 */
static void test_widest_push_block_picks_r7_by_value(void)
{
	memset(stack, 0, STACK_WORDS * 4u);

	/* F0: push {r4, r5, r6, r7, r8, r9, sl, fp, lr} — the saved r7 is
	 * five words below the saved LR, the saved fp is right below it and
	 * holds a pointer into F2's frame. */
	stack[8] = 0x44444444u;	 /* saved r4 */
	stack[9] = 0x55555555u;	 /* saved r5 */
	stack[10] = 0x66666666u; /* saved r6 */
	/* saved r7 (K = 0) */
	stack[11] = (uint32_t)(uintptr_t)&stack[17];
	stack[12] = 0x88888888u; /* saved r8 */
	stack[13] = 0x99999999u; /* saved r9 */
	stack[14] = 0xAAAA4444u; /* saved sl */
	/* saved fp: not an r7 */
	stack[15] = (uint32_t)(uintptr_t)&stack[30];
	stack[16] = ret_addr(0);

	/* F1: two words of locals, then the same nine-word push block */
	stack[17] = 0xEEEEEEEEu;
	stack[18] = 0xEEEEEEEEu;
	stack[19] = 0x44444444u; /* saved r4 */
	stack[20] = 0x55555555u; /* saved r5 */
	stack[21] = 0x66666666u; /* saved r6 */
	/* saved r7 (K = 0) */
	stack[22] = (uint32_t)(uintptr_t)&stack[28];
	stack[23] = 0x88888888u; /* saved r8 */
	stack[24] = 0x99999999u; /* saved r9 */
	stack[25] = 0xAAAA4444u; /* saved sl */
	/* saved fp: not an r7 */
	stack[26] = (uint32_t)(uintptr_t)&stack[45];
	stack[27] = ret_addr(1);

	/* F2: three words of locals, then the same nine-word push block */
	stack[28] = 0xEEEEEEEEu;
	stack[29] = 0xEEEEEEEEu;
	stack[30] = 0xEEEEEEEEu;
	stack[31] = 0x44444444u; /* saved r4 */
	stack[32] = 0x55555555u; /* saved r5 */
	stack[33] = 0x66666666u; /* saved r6 */
	/* saved r7 (K = 0) */
	stack[34] = (uint32_t)(uintptr_t)&stack[40];
	stack[35] = 0x88888888u; /* saved r8 */
	stack[36] = 0x99999999u; /* saved r9 */
	stack[37] = 0xAAAA4444u; /* saved sl */
	stack[38] = 0xBBBB4444u; /* saved fp */
	stack[39] = ret_addr(2);

	uintptr_t out[8];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[40], out, 8);
	CHECK(n == 3);
	for (int k = 0; k < n && k < 3; k++)
		CHECK(out[k] == (ret_addr((unsigned)k) & ~1u));
}

/* A stale {r7, lr} pair in an outer frame's locals is rejected when it
 * sits at or below the address the last accepted saved r7 points to,
 * even though its own saved-r7 value lies above that saved r7. */
static void test_stale_pair_below_accepted_r7(void)
{
	memset(stack, 0, STACK_WORDS * 4u);
	/* F0: push {r7, lr}; the saved r7 is F1's r7 = F1's SP + 16 (K = 16) */
	stack[8] = (uint32_t)(uintptr_t)&stack[14];
	stack[9] = ret_addr(0);
	/* F1 locals 10..15 hold a stale pair whose saved r7 fits its own
	 * window (K = 8) and lies above F0's saved r7 */
	stack[11] = (uint32_t)(uintptr_t)&stack[15];
	stack[12] = ret_addr(5);
	/* F1: push {r7, lr} */
	stack[16] = (uint32_t)(uintptr_t)&stack[18];
	stack[17] = ret_addr(1);
	uintptr_t out[8];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[18], out, 8);
	CHECK(n == 2);
	CHECK(n < 1 || out[0] == (ret_addr(0) & ~1u));
	CHECK(n < 2 || out[1] == (ret_addr(1) & ~1u));
}

/* A word below the saved r7 can also fall in the value window (here a
 * saved r6 holding a pointer further up).  The pick is the value closest
 * to the saved-LR slot, not the lowest word: anchoring on the r6 would put
 * the caller's push block below the anchor and drop the caller. */
static void test_r7_picked_over_a_lower_candidate(void)
{
	memset(stack, 0, STACK_WORDS * 4u);
	/* F0: push {r6, r7, r8, lr} */
	stack[8] = (uint32_t)(uintptr_t)&stack[22]; /* saved r6, K = 40 */
	stack[9] = (uint32_t)(uintptr_t)&stack[12]; /* saved r7, K = 0 */
	stack[10] = 0x88888888u;		    /* saved r8 */
	stack[11] = ret_addr(0);
	/* F1: two words of locals, then push {r7, lr} */
	stack[14] = (uint32_t)(uintptr_t)&stack[16];
	stack[15] = ret_addr(1);
	uintptr_t out[8];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[16], out, 8);
	CHECK(n == 2);
	CHECK(n < 2 || out[1] == (ret_addr(1) & ~1u));
}

/* The push block holds at most five words below the saved LR; a fitting
 * value six words below it is not a saved r7. */
static void test_push_block_is_five_words(void)
{
	memset(stack, 0, STACK_WORDS * 4u);
	stack[8] = (uint32_t)(uintptr_t)&stack[15];
	stack[14] = ret_addr(0);
	uintptr_t out[8];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[16], out, 8);
	CHECK(n == 0);
}

/* The push block is searched only down to the scan start: a word in the
 * exception frame below it is never taken as the saved r7, even when its
 * value would pass as one. */
static void test_push_block_stops_at_scan_start(void)
{
	memset(stack, 0, STACK_WORDS * 4u);
	/* stack[0..7] is the exception frame; the scan starts at stack[8]. */
	stack[7] = (uint32_t)(uintptr_t)&stack[9];
	stack[8] = ret_addr(0);
	uintptr_t out[8];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[16], out, 8);
	CHECK(n == 0);
}

/* Never-written locals still hold the RTOS stack fill pattern; that must
 * not end the scan (it used to). */
static void test_fill_pattern_inside_frames(void)
{
	unsigned top = build_frames(5, 4, FILL);
	uintptr_t out[16];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[top], out, 16);
	CHECK(n == 5);
}

/* Valid-looking pairs beyond the stack top (neighbouring memory) must not
 * be reported. */
static void test_stops_at_stack_top(void)
{
	(void)build_frames(6, 2, 0);
	/* Frames 0..2 occupy 8 + 3 * (2 + 2) words; the stack ends there. */
	unsigned top = 8 + 3 * 4;
	uintptr_t out[16];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[top], out, 16);
	CHECK(n == 3);
}

static void test_max_slots(void)
{
	unsigned top = build_frames(6, 1, 0);
	uintptr_t out[4];
	int n = ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), (uintptr_t)stack,
				       (uintptr_t)&stack[top], out, 4);
	CHECK(n == 4);
}

static void test_psp_outside_stack(void)
{
	unsigned top = build_frames(3, 1, 0);
	uintptr_t out[8];
	uintptr_t lo = (uintptr_t)&stack[16], hi = (uintptr_t)&stack[top];
	CHECK(ove_arm_backtrace_walk((uintptr_t)stack, text_lo(), text_hi(), lo, hi, out, 8) == 0);
	CHECK(ove_arm_backtrace_walk(hi, text_lo(), text_hi(), lo, hi, out, 8) == 0);
}

/* Exception frame reaching past the stack top: nothing to scan, and no
 * out-of-range read (scan start beyond scan end). */
static void test_frame_at_stack_top(void)
{
	(void)build_frames(3, 1, 0);
	uintptr_t out[8];
	uintptr_t psp = (uintptr_t)&stack[STACK_WORDS - 4];
	CHECK(ove_arm_backtrace_walk(psp, text_lo(), text_hi(), (uintptr_t)stack,
				     (uintptr_t)&stack[STACK_WORDS], out, 8) == 0);
}

/* The stack ends right at an inaccessible page and is full of valid
 * frames up to its last word: the scan must stop at the stack top without
 * touching the next page (it used to run a fixed 1 KiB past PSP), for
 * every PSP near the top and for an unaligned top. */
static void test_no_read_past_stack_top(void)
{
	const size_t pg = 4096u;
	uint8_t *m = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOW, -1, 0);
	if (m == MAP_FAILED || (uintptr_t)m + 2 * pg > UINT32_MAX) {
		printf("test_no_read_past_stack_top: SKIP (no 32-bit mapping)\n");
		return;
	}
	CHECK(mprotect(m + pg, pg, PROT_NONE) == 0);
	uint32_t *stk = (uint32_t *)m;
	const unsigned words = (unsigned)(pg / 4u);
	uint32_t ra = ret_addr(0);
	/* {local, saved r7, saved lr} frames; the last lr is the top word. */
	unsigned frames = 0;
	memset(stk, 0, pg);
	for (unsigned w = 8u + (words - 8u) % 3u; w + 2u < words; w += 3u, frames++) {
		stk[w + 1u] = (uint32_t)(uintptr_t)&stk[w + 3u]; /* saved r7, K = 0 */
		stk[w + 2u] = ra;
	}
	uintptr_t lo = (uintptr_t)m, hi = (uintptr_t)(m + pg);
	uintptr_t out[512];

	for (uintptr_t psp = hi - 64u; psp <= hi; psp += 4u) {
		CHECK(ove_arm_backtrace_walk(psp, text_lo(), text_hi(), lo, hi, out, 8) <= 4);
		CHECK(ove_arm_backtrace_walk(psp, text_lo(), text_hi(), lo, hi - 1u, out, 8) <= 4);
	}
	/* Whole stack: every frame up to the last word, then stop. */
	CHECK(ove_arm_backtrace_walk(lo, text_lo(), text_hi(), lo, hi, out, 512) == (int)frames);
	munmap(m, 2 * pg);
}

static void test_lr_is_post_bl(void)
{
	uint32_t lr = ret_addr(7) & ~1u;
	CHECK(ove_arm_backtrace_lr_is_post_bl(lr, text_lo(), text_hi()));
	/* blx reg (16-bit, 0x4780 | Rm << 3) */
	uintptr_t blx = text_lo() + 0x900u;
	text[(blx - text_lo()) / 2] = 0x4798; /* blx r3 */
	CHECK(ove_arm_backtrace_lr_is_post_bl(blx + 2u, text_lo(), text_hi()));
	/* data word, not after a call */
	text[(0xA00u) / 2] = 0x1234;
	text[(0xA00u) / 2 + 1] = 0x5678;
	CHECK(!ove_arm_backtrace_lr_is_post_bl(text_lo() + 0xA04u, text_lo(), text_hi()));
}

/* Lay a handler-mode exception frame at word @off: @xpsr, @pc and @lr,
 * and, when @exc_return is not 0, the sampler's saved EXC_RETURN in the
 * word below it. */
static void put_frame(unsigned off, uint32_t xpsr, uint32_t pc, uint32_t lr, uint32_t exc_return)
{
	stack[off + OVE_ARM_EXC_R0] = 0x11111111u;
	stack[off + OVE_ARM_EXC_R1] = 0x22222222u;
	stack[off + OVE_ARM_EXC_R2] = 0x33333333u;
	stack[off + OVE_ARM_EXC_R3] = 0x44444444u;
	stack[off + OVE_ARM_EXC_R12] = 0x55555555u;
	stack[off + OVE_ARM_EXC_LR] = lr;
	stack[off + OVE_ARM_EXC_PC] = pc;
	stack[off + OVE_ARM_EXC_XPSR] = xpsr;
	if (exc_return)
		stack[off - 1u] = exc_return;
}

#define THUMB (1u << 24)
#define SAMPLER_EXC 25u /* the sampler's own exception number */

static const uint32_t *find(unsigned limit_word, unsigned max_words)
{
	return ove_arm_find_exc_frame((uintptr_t)stack, (uintptr_t)&stack[limit_word], text_lo(),
				      text_hi(), max_words, SAMPLER_EXC);
}

static void test_find_exc_frame(void)
{
	memset(stack, FILL, STACK_WORDS * 4u);
	put_frame(20, THUMB | 15u, (uint32_t)text_lo() + 0x300u, 0xFFFFFFF9u, 0xFFFFFFF1u);
	const uint32_t *f = find(STACK_WORDS, 256);
	CHECK(f == &stack[20]);
	CHECK(f && (f[OVE_ARM_EXC_XPSR] & 0x1FFu) == 15u);

	/* A frame with FP state is anchored by 0xFFFFFFE1. */
	memset(stack, FILL, STACK_WORDS * 4u);
	put_frame(20, THUMB | 14u, (uint32_t)text_lo() + 0x300u, 0xFFFFFFF9u, 0xFFFFFFE1u);
	CHECK(find(STACK_WORDS, 256) == &stack[20]);
}

/* What lies between the stack pointer and the frame: the sampler's own
 * locals, which can look like a frame but are not anchored, and frames
 * that fail the checks. */
static void test_find_exc_frame_skips_noise(void)
{
	memset(stack, FILL, STACK_WORDS * 4u);
	uint32_t pc = (uint32_t)text_lo() + 0x300u;
	/* Locals that look like a frame (a pattern seen on QEMU). */
	put_frame(8, 0x010007d0u, pc, (uint32_t)text_lo() + 0x200u, 0);
	/* Anchored, but thread mode (IPSR 0). */
	put_frame(18, THUMB, pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	/* Anchored, but the sampler's own exception number. */
	put_frame(28, THUMB | SAMPLER_EXC, pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	/* Anchored, but a reserved exception number. */
	put_frame(38, THUMB | 9u, pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	/* Anchored, but no Thumb bit. */
	put_frame(48, 15u, pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	/* Anchored, but reserved xPSR bits set. */
	put_frame(58, THUMB | (1u << 20) | 15u, pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	/* Anchored, but the PC is outside .text. */
	put_frame(68, THUMB | 15u, 0x12345678u, 0xFFFFFFF9u, 0xFFFFFFF1u);
	/* Anchored, but exception 1, 7 or 13, which cannot be preempted
	 * handlers. */
	put_frame(88, THUMB | 1u, pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	put_frame(98, THUMB | 7u, pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	put_frame(108, THUMB | 13u, pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	/* Anchored, but the PC is just past .text. */
	put_frame(118, THUMB | 15u, (uint32_t)text_hi(), 0xFFFFFFF9u, 0xFFFFFFF1u);
	/* The real frame: PendSV, which uses lr as a scratch register. */
	put_frame(128, THUMB | 14u, pc + 0x100u, 0x2010acdcu, 0xFFFFFFF1u);
	CHECK(find(STACK_WORDS, 256) == &stack[128]);
	/* An external interrupt above 255 still counts: IPSR is 9 bits, and
	 * 256 + the sampler's own number is a different exception. */
	memset(stack, FILL, STACK_WORDS * 4u);
	put_frame(20, THUMB | (256u + SAMPLER_EXC), pc, 0xFFFFFFF9u, 0xFFFFFFF1u);
	CHECK(find(STACK_WORDS, 256) == &stack[20]);
}

static void test_find_exc_frame_bounds(void)
{
	memset(stack, FILL, STACK_WORDS * 4u);
	put_frame(40, THUMB | 15u, (uint32_t)text_lo() + 0x300u, 0xFFFFFFF9u, 0xFFFFFFF1u);

	/* Frame at or above the stack limit must not be read/reported. */
	CHECK(!find(40, 256));
	/* max_words stops the scan before the frame. */
	CHECK(!find(STACK_WORDS, 32));
	/* A full-window scan finds it. */
	CHECK(find(STACK_WORDS, 256) == &stack[40]);
	/* The frame may end exactly at the limit, never beyond it. */
	memset(stack, FILL, STACK_WORDS * 4u);
	put_frame(32, THUMB | 15u, (uint32_t)text_lo() + 0x300u, 0xFFFFFFF9u, 0xFFFFFFF1u);
	CHECK(find(40, 256) == &stack[32]);
	CHECK(!find(39, 256));
	/* Unaligned and empty windows are rejected. */
	CHECK(!ove_arm_find_exc_frame((uintptr_t)stack + 1u, (uintptr_t)&stack[STACK_WORDS],
				      text_lo(), text_hi(), 256, SAMPLER_EXC));
	CHECK(!ove_arm_find_exc_frame((uintptr_t)&stack[STACK_WORDS],
				      (uintptr_t)&stack[STACK_WORDS], text_lo(), text_hi(), 256,
				      SAMPLER_EXC));
}

int main(void)
{
	void *t = mmap(NULL, TEXT_BYTES, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOW, -1, 0);
	void *s = mmap(NULL, STACK_WORDS * 4u, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOW, -1, 0);
	if (t == MAP_FAILED || s == MAP_FAILED || (uintptr_t)t > UINT32_MAX ||
	    (uintptr_t)s > UINT32_MAX) {
		printf("test_arm_backtrace: SKIP (no 32-bit mapping)\n");
		return 0;
	}
	text = (uint16_t *)t;
	stack = (uint32_t *)s;

	test_full_chain();
	test_wide_push_block();
	test_r7_picked_by_value();
	test_widest_push_block_picks_r7_by_value();
	test_stale_pair_below_accepted_r7();
	test_r7_picked_over_a_lower_candidate();
	test_push_block_is_five_words();
	test_push_block_stops_at_scan_start();
	test_fill_pattern_inside_frames();
	test_stops_at_stack_top();
	test_max_slots();
	test_psp_outside_stack();
	test_frame_at_stack_top();
	test_no_read_past_stack_top();
	test_lr_is_post_bl();
	test_find_exc_frame();
	test_find_exc_frame_skips_noise();
	test_find_exc_frame_bounds();

	if (failures) {
		fprintf(stderr, "test_arm_backtrace: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_arm_backtrace: OK\n");
	return 0;
}
