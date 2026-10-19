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
	test_fill_pattern_inside_frames();
	test_stops_at_stack_top();
	test_max_slots();
	test_psp_outside_stack();
	test_frame_at_stack_top();
	test_no_read_past_stack_top();
	test_lr_is_post_bl();

	if (failures) {
		fprintf(stderr, "test_arm_backtrace: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_arm_backtrace: OK\n");
	return 0;
}
