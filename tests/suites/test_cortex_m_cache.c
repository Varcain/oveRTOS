/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host-side tests for the pure cache-geometry and line-span calculations used
 * by bounded Cortex-M executable publication.
 */

#include "../framework/ove_test.h"

#include "ove_cortex_m_cache.h"

static uint32_t ccsidr(size_t line_size, size_t ways, size_t sets)
{
	unsigned line_field = 0u;
	for (size_t n = line_size; n > 16u; n >>= 1u)
		line_field++;
	return (uint32_t)line_field | ((uint32_t)(ways - 1u) << 3) | ((uint32_t)(sets - 1u) << 13);
}

static void test_f746_cache_shapes(void **state)
{
	(void)state;
	struct ove_cortex_m_cache_shape shape;

	assert_int_equal(ove_cortex_m_cache_shape_decode(ccsidr(32u, 4u, 128u), &shape), 0);
	assert_int_equal(shape.line_size, 32u);
	assert_int_equal(shape.ways, 4u);
	assert_int_equal(shape.sets, 128u);
	assert_int_equal(shape.size, 16u * 1024u);

	assert_int_equal(ove_cortex_m_cache_shape_decode(ccsidr(32u, 2u, 256u), &shape), 0);
	assert_int_equal(shape.line_size, 32u);
	assert_int_equal(shape.ways, 2u);
	assert_int_equal(shape.sets, 256u);
	assert_int_equal(shape.size, 16u * 1024u);
}

static void test_synthetic_cache_shapes(void **state)
{
	(void)state;
	struct ove_cortex_m_cache_shape shape;

	assert_int_equal(ove_cortex_m_cache_shape_decode(ccsidr(16u, 1u, 1u), &shape), 0);
	assert_int_equal(shape.line_size, 16u);
	assert_int_equal(shape.size, 16u);
	assert_int_equal(ove_cortex_m_cache_shape_decode(ccsidr(64u, 8u, 64u), &shape), 0);
	assert_int_equal(shape.line_size, 64u);
	assert_int_equal(shape.size, 32u * 1024u);
	assert_int_equal(ove_cortex_m_cache_shape_decode(0u, NULL), -1);
}

static void test_line_span_rounds_outward(void **state)
{
	(void)state;
	struct ove_cortex_m_cache_line_span span;

	assert_int_equal(ove_cortex_m_cache_line_span(0x1003u, 64u, 32u, &span), 0);
	assert_int_equal(span.first, 0x1000u);
	assert_int_equal(span.count, 3u);

	assert_int_equal(ove_cortex_m_cache_line_span(0x1000u, 32u, 32u, &span), 0);
	assert_int_equal(span.first, 0x1000u);
	assert_int_equal(span.count, 1u);
}

static void test_line_span_rejects_invalid_ranges(void **state)
{
	(void)state;
	struct ove_cortex_m_cache_line_span span;

	assert_int_equal(ove_cortex_m_cache_line_span(0u, 0u, 32u, &span), -1);
	assert_int_equal(ove_cortex_m_cache_line_span(0u, 1u, 0u, &span), -1);
	assert_int_equal(ove_cortex_m_cache_line_span(0u, 1u, 24u, &span), -1);
	assert_int_equal(ove_cortex_m_cache_line_span(UINTPTR_MAX - 7u, 10u, 32u, &span), -1);
	assert_int_equal(ove_cortex_m_cache_line_span(0u, 1u, 32u, NULL), -1);
}

int test_cortex_m_cache_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_f746_cache_shapes),
		cmocka_unit_test(test_synthetic_cache_shapes),
		cmocka_unit_test(test_line_span_rounds_outward),
		cmocka_unit_test(test_line_span_rejects_invalid_ranges),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
