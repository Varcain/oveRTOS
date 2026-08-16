/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host checks for oveRTOS-owned STM32F746 memory policy. Pure architectural
 * decoding and range behavior are tested by canonical LXP.
 */

#include "../framework/ove_test.h"

#include "lxp/arch/cortex_m_cache.h"
#include "ove_lxp_memory_contract.h"

static uint32_t ccsidr(size_t line_size, size_t ways, size_t sets)
{
	unsigned line_field = 0u;
	for (size_t n = line_size; n > 16u; n >>= 1u)
		line_field++;
	return (uint32_t)line_field | ((uint32_t)(ways - 1u) << 3) |
	       ((uint32_t)(sets - 1u) << 13);
}

static void test_stm32f746_cache_policy(void **state)
{
	(void)state;
	struct lxp_cortex_m_cache_shape shape;
	const lxp_cpu_memory_contract_t contract =
		OVE_LXP_MEMORY_CONTRACT_STM32F746_INITIALIZER;

	assert_int_equal(lxp_cortex_m_cache_shape_decode(ccsidr(32u, 4u, 32u), &shape), 0);
	assert_int_equal(shape.line_size, contract.dcache_line_size);
	assert_int_equal(shape.size, contract.dcache_size);
	assert_int_equal(lxp_cortex_m_cache_shape_decode(ccsidr(32u, 2u, 64u), &shape), 0);
	assert_int_equal(shape.line_size, contract.icache_line_size);
	assert_int_equal(shape.size, contract.icache_size);
	assert_int_equal(contract.model, LXP_CPU_MEM_COHERENT_SAME_ATTRS);
	assert_int_equal(contract.normal_attrs, LXP_CPU_MEM_ATTR_NORMAL_WBWA_NSH);
}

static void test_uncached_policy(void **state)
{
	(void)state;
	const lxp_cpu_memory_contract_t contract = OVE_LXP_MEMORY_CONTRACT_UNCACHED_INITIALIZER;

	assert_int_equal(contract.model, LXP_CPU_MEM_UNCACHED);
	assert_int_equal(contract.normal_attrs, LXP_CPU_MEM_ATTR_NORMAL_NC_NSH);
	assert_int_equal(contract.flags, 0u);
}

int test_lxp_memory_policy_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_stm32f746_cache_policy),
		cmocka_unit_test(test_uncached_policy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
