/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "ove/arena.h"

#include <string.h>

#define ARENA_BYTES 4096u

static uint8_t s_buf[ARENA_BYTES] __attribute__((aligned(OVE_ARENA_ALIGN)));
static ove_arena_t s_arena;

static int arena_setup(void **state)
{
	(void)state;
	assert_int_equal(ove_arena_init(&s_arena, s_buf, sizeof(s_buf)), OVE_OK);
	return 0;
}

/* ── init ────────────────────────────────────────────────────────────── */

static void test_arena_init_basic(void **state)
{
	(void)state;
	assert_int_equal(ove_arena_used(&s_arena), 0);
	assert_int_equal(ove_arena_high_water(&s_arena), 0);
	assert_true(ove_arena_capacity(&s_arena) > 0);
	assert_true(ove_arena_capacity(&s_arena) <= sizeof(s_buf));
}

static void test_arena_init_rejects_bad(void **state)
{
	(void)state;
	ove_arena_t a;
	uint8_t small[8];
	assert_int_equal(ove_arena_init(NULL, s_buf, sizeof(s_buf)), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_arena_init(&a, NULL, sizeof(s_buf)), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_arena_init(&a, small, sizeof(small)), OVE_ERR_NO_MEMORY);
}

/* ── alloc ───────────────────────────────────────────────────────────── */

static void test_arena_alloc_alignment(void **state)
{
	(void)state;
	const size_t sizes[] = {1, 7, 16, 17, 100, 333};
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		void *p = ove_arena_alloc(&s_arena, sizes[i]);
		assert_non_null(p);
		assert_int_equal((uintptr_t)p % OVE_ARENA_ALIGN, 0);
		assert_true(ove_arena_owns(&s_arena, p));
	}
}

static void test_arena_alloc_no_overlap(void **state)
{
	(void)state;
	uint8_t *a = ove_arena_alloc(&s_arena, 200);
	uint8_t *b = ove_arena_alloc(&s_arena, 200);
	uint8_t *c = ove_arena_alloc(&s_arena, 200);
	assert_non_null(a);
	assert_non_null(b);
	assert_non_null(c);

	memset(a, 0xAA, 200);
	memset(b, 0xBB, 200);
	memset(c, 0xCC, 200);

	/* No write bled into a neighbour. */
	for (int i = 0; i < 200; i++) {
		assert_int_equal(a[i], 0xAA);
		assert_int_equal(b[i], 0xBB);
		assert_int_equal(c[i], 0xCC);
	}
}

static void test_arena_calloc_zeroes(void **state)
{
	(void)state;
	uint8_t *p = ove_arena_calloc(&s_arena, 256);
	assert_non_null(p);
	for (int i = 0; i < 256; i++)
		assert_int_equal(p[i], 0);
}

/* ── accounting ──────────────────────────────────────────────────────── */

static void test_arena_used_tracking(void **state)
{
	(void)state;
	assert_int_equal(ove_arena_used(&s_arena), 0);

	void *a = ove_arena_alloc(&s_arena, 100);
	size_t after_a = ove_arena_used(&s_arena);
	assert_true(after_a > 0);

	void *b = ove_arena_alloc(&s_arena, 100);
	assert_true(ove_arena_used(&s_arena) > after_a);

	size_t peak = ove_arena_used(&s_arena);
	ove_arena_free(&s_arena, a);
	ove_arena_free(&s_arena, b);

	assert_int_equal(ove_arena_used(&s_arena), 0);
	assert_int_equal(ove_arena_high_water(&s_arena), peak);
}

/* ── exhaustion ──────────────────────────────────────────────────────── */

static void test_arena_exhaustion(void **state)
{
	(void)state;
	void *last = NULL;
	int allocs = 0;
	for (;;) {
		void *p = ove_arena_alloc(&s_arena, 64);
		if (!p)
			break;
		last = p;
		allocs++;
		assert_true(allocs < 1000); /* bounded — must terminate */
	}
	assert_true(allocs > 0);

	/* Freeing one slot lets the next allocation through again. */
	ove_arena_free(&s_arena, last);
	assert_non_null(ove_arena_alloc(&s_arena, 64));
}

/* ── coalescing ──────────────────────────────────────────────────────── */

static void test_arena_free_coalesces(void **state)
{
	(void)state;
	void *a = ove_arena_alloc(&s_arena, 1000);
	void *b = ove_arena_alloc(&s_arena, 1000);
	void *c = ove_arena_alloc(&s_arena, 1000);
	assert_non_null(a);
	assert_non_null(b);
	assert_non_null(c);

	/* A 3000-byte block fits only if the three frees coalesce back into
	 * one contiguous extent — it is larger than any single freed block. */
	ove_arena_free(&s_arena, a);
	ove_arena_free(&s_arena, c);
	ove_arena_free(&s_arena, b);

	void *big = ove_arena_alloc(&s_arena, 3000);
	assert_non_null(big);
}

/* ── safety ──────────────────────────────────────────────────────────── */

static void test_arena_double_free_safe(void **state)
{
	(void)state;
	void *p = ove_arena_alloc(&s_arena, 128);
	assert_non_null(p);
	ove_arena_free(&s_arena, p);
	ove_arena_free(&s_arena, p); /* second free must not corrupt state */
	assert_int_equal(ove_arena_used(&s_arena), 0);
	assert_non_null(ove_arena_alloc(&s_arena, 128));
}

static void test_arena_owns(void **state)
{
	(void)state;
	int stack_var = 0;
	void *p = ove_arena_alloc(&s_arena, 32);
	assert_non_null(p);
	assert_true(ove_arena_owns(&s_arena, p));
	assert_false(ove_arena_owns(&s_arena, &stack_var));
	assert_false(ove_arena_owns(&s_arena, NULL));
}

/* ── reset ───────────────────────────────────────────────────────────── */

static void test_arena_reset_reclaims(void **state)
{
	(void)state;
	void *big = ove_arena_alloc(&s_arena, 2000);
	assert_non_null(big);
	(void)ove_arena_alloc(&s_arena, 500);

	ove_arena_reset(&s_arena);
	assert_int_equal(ove_arena_used(&s_arena), 0);

	/* The full extent is available again after reset. */
	assert_non_null(ove_arena_alloc(&s_arena, 2000));
}

int test_arena_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_arena_init_basic, arena_setup),
		cmocka_unit_test_setup(test_arena_init_rejects_bad, arena_setup),
		cmocka_unit_test_setup(test_arena_alloc_alignment, arena_setup),
		cmocka_unit_test_setup(test_arena_alloc_no_overlap, arena_setup),
		cmocka_unit_test_setup(test_arena_calloc_zeroes, arena_setup),
		cmocka_unit_test_setup(test_arena_used_tracking, arena_setup),
		cmocka_unit_test_setup(test_arena_exhaustion, arena_setup),
		cmocka_unit_test_setup(test_arena_free_coalesces, arena_setup),
		cmocka_unit_test_setup(test_arena_double_free_safe, arena_setup),
		cmocka_unit_test_setup(test_arena_owns, arena_setup),
		cmocka_unit_test_setup(test_arena_reset_reclaims, arena_setup),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
