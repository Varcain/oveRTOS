/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * On-target loader test: load a real ELF32/ARM module into RAM and *execute*
 * its functions on the Cortex-M, exercising reloc-free code and an
 * R_ARM_ABS32 relocation (an internal global's address materialised from a
 * .text literal pool). Lives outside tests/suites/ (not a host-categorised
 * suite) and is dispatched only from the NuttX runner.
 */

#include "framework/ove_test.h"
#include "ove/loader.h"

#include "loader_mod_exec_image.h" /* ove_loader_test_exec_arm[], _len */

/* Destination for the loaded module. NuttX flat build: RAM is executable. */
static uint8_t s_region[8192] __attribute__((aligned(8)));

static void test_loader_target_exec(void **state)
{
	(void)state;
	ove_module_t mod;
	assert_int_equal(ove_loader_load(&mod, ove_loader_test_exec_arm,
					 ove_loader_test_exec_arm_len, s_region, sizeof(s_region),
					 NULL, 0),
			 OVE_OK);
	assert_int_equal(mod.is_elf64, 0);

	/* Function symbols come back with the Thumb bit set, so they are
	 * directly callable as function pointers. */
	int (*m_add)(int, int) = (int (*)(int, int))ove_loader_sym(&mod, "m_add");
	int (*m_mul)(int, int) = (int (*)(int, int))ove_loader_sym(&mod, "m_mul");
	int (*m_neg)(int) = (int (*)(int))ove_loader_sym(&mod, "m_neg");
	assert_non_null(m_add);
	assert_non_null(m_mul);
	assert_non_null(m_neg);

	/* Reloc-free leaf functions execute on the Cortex-M. */
	assert_int_equal(m_add(3, 4), 7);
	assert_int_equal(m_mul(6, 7), 42);
	assert_int_equal(m_neg(5), -5);

	/* R_ARM_ABS32 (literal pool): the loaded code reads its own global,
	 * proving the relocation was applied correctly before execution. */
	int (*m_read_global)(void) = (int (*)(void))ove_loader_sym(&mod, "m_read_global");
	assert_non_null(m_read_global);
	assert_int_equal(m_read_global(), 0x1234);
}

int test_loader_target_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_loader_target_exec),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
