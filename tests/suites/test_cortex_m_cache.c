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
#include "ove_cortex_m_mpu.h"

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

static uint32_t rasr(unsigned log2_size, uint8_t subregions, uint8_t texscb, uint8_t access,
		     uint8_t execute_never)
{
	return 1u | ((log2_size - 1u) << 1) | ((uint32_t)subregions << 8) |
	       ((uint32_t)texscb << 16) | ((uint32_t)access << 24) |
	       ((uint32_t)execute_never << 28);
}

static void test_mpu_region_decode_and_match(void **state)
{
	(void)state;
	struct ove_cortex_m_mpu_region region;

	assert_int_equal(ove_cortex_m_mpu_region_decode(0xc0000001u, rasr(23u, 1u, 0x0bu, 1u, 1u),
							&region),
			 0);
	assert_int_equal(region.base, 0xc0000000u);
	assert_int_equal(region.size, 8u * 1024u * 1024u);
	assert_int_equal(region.subregion_disable, 1u);
	assert_int_equal(region.texscb, 0x0bu);
	assert_int_equal(region.access, 1u);
	assert_int_equal(region.execute_never, 1u);
	assert_true(ove_cortex_m_mpu_region_matches(&region, 0xc0000000u, 8u * 1024u * 1024u, 1u,
						    0x0bu, 1u, 1u));
	assert_false(ove_cortex_m_mpu_region_matches(&region, 0xc0000000u, 8u * 1024u * 1024u, 0u,
						     0x0bu, 1u, 1u));
}

static void test_mpu_subregion_ranges(void **state)
{
	(void)state;
	struct ove_cortex_m_mpu_region region;
	assert_int_equal(ove_cortex_m_mpu_region_decode(0xc0000000u, rasr(23u, 1u, 0x0bu, 1u, 1u),
							&region),
			 0);

	assert_false(ove_cortex_m_mpu_region_contains(&region, 0xc0000000u, 0x1000u));
	assert_false(ove_cortex_m_mpu_region_overlaps_enabled(&region, 0xc0000000u, 0x100000u));
	assert_true(ove_cortex_m_mpu_region_contains(&region, 0xc0100000u, 0x700000u));
	assert_true(ove_cortex_m_mpu_region_overlaps_enabled(&region, 0xc00ff000u, 0x2000u));
	assert_false(ove_cortex_m_mpu_region_contains(&region, 0xc00ff000u, 0x2000u));
}

static void test_mpu_rejects_invalid_descriptors_and_ranges(void **state)
{
	(void)state;
	struct ove_cortex_m_mpu_region region;
	assert_int_equal(ove_cortex_m_mpu_region_decode(0u, 1u | (3u << 1), &region), -1);
	assert_int_equal(ove_cortex_m_mpu_region_decode(0u, 0u, &region), 0);
	assert_false(ove_cortex_m_mpu_region_contains(&region, 0u, 1u));
	assert_int_equal(ove_cortex_m_mpu_region_decode(0u, 0u, NULL), -1);
}

static void test_mpu_effective_mapping_rejects_higher_overlay(void **state)
{
	(void)state;
	const struct ove_cortex_m_mpu_expectation expected = {
		.base = 0xc0100000u,
		.size = 256u * 1024u,
		.texscb = 0x0bu,
		.access = 3u,
		.execute_never = 1u,
	};
	struct ove_cortex_m_mpu_snapshot snapshot = {
		.ctrl = OVE_CORTEX_M_MPU_CTRL_ENABLE | OVE_CORTEX_M_MPU_CTRL_PRIVDEFENA,
		.count = 8u,
	};
	assert_int_equal(ove_cortex_m_mpu_region_decode(0xc0100000u, rasr(18u, 0u, 0x0bu, 3u, 1u),
							&snapshot.regions[2]),
			 0);
	assert_true(ove_cortex_m_mpu_descriptor_matches(0xc0100000u, rasr(18u, 0u, 0x0bu, 3u, 1u),
							&expected));
	assert_true(ove_cortex_m_mpu_snapshot_effective_matches(&snapshot, &expected));

	/* A higher-numbered stale region wins even though region 2 still looks right. */
	assert_int_equal(ove_cortex_m_mpu_region_decode(0xc0100000u, rasr(18u, 0u, 0x08u, 3u, 1u),
							&snapshot.regions[5]),
			 0);
	assert_false(ove_cortex_m_mpu_snapshot_effective_matches(&snapshot, &expected));
	snapshot.regions[5].enabled = 0u;
	assert_true(ove_cortex_m_mpu_snapshot_effective_matches(&snapshot, &expected));

	assert_false(ove_cortex_m_mpu_snapshot_effective_matches(NULL, &expected));
	assert_false(ove_cortex_m_mpu_snapshot_effective_matches(&snapshot, NULL));

	/* A broader RW+XN descriptor may be the effective mapping for the tail
	 * beyond a higher-priority RX prefix, but not for a range it only partly
	 * covers or when that prefix overlaps the requested tail. */
	const struct ove_cortex_m_mpu_expectation tail = {
		.base = 0x20001000u,
		.size = 0x1000u,
		.texscb = 0x0bu,
		.access = 3u,
		.execute_never = 1u,
	};
	assert_int_equal(ove_cortex_m_mpu_region_decode(0x20000000u, rasr(15u, 0u, 0x0bu, 3u, 1u),
							&snapshot.regions[0]),
			 0);
	assert_int_equal(ove_cortex_m_mpu_region_decode(0x20000000u, rasr(11u, 0u, 0x0bu, 6u, 0u),
							&snapshot.regions[1]),
			 0);
	assert_true(ove_cortex_m_mpu_snapshot_effective_contains(&snapshot, &tail));
	assert_int_equal(ove_cortex_m_mpu_region_decode(0x20001000u, rasr(11u, 0u, 0x0bu, 6u, 0u),
							&snapshot.regions[1]),
			 0);
	assert_false(ove_cortex_m_mpu_snapshot_effective_contains(&snapshot, &tail));
}

int test_cortex_m_cache_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_f746_cache_shapes),
		cmocka_unit_test(test_synthetic_cache_shapes),
		cmocka_unit_test(test_line_span_rounds_outward),
		cmocka_unit_test(test_line_span_rejects_invalid_ranges),
		cmocka_unit_test(test_mpu_region_decode_and_match),
		cmocka_unit_test(test_mpu_subregion_ranges),
		cmocka_unit_test(test_mpu_rejects_invalid_descriptors_and_ranges),
		cmocka_unit_test(test_mpu_effective_mapping_rejects_higher_overlay),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
