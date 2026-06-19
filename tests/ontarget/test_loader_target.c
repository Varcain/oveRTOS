/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * On-target loader test: load a real ELF32/ARM module into RAM and *execute*
 * its functions on the Cortex-M. Exercises reloc-free code, an R_ARM_ABS32
 * relocation (internal global via a .text literal pool), and an R_ARM_THM_CALL
 * to a far firmware import routed through a loader-generated veneer. Lives
 * outside tests/suites/ (not a host-categorised suite) and is dispatched only
 * from the NuttX runner.
 */

#include "framework/ove_test.h"
#include "ove/loader.h"

#include "loader_mod_exec_image.h" /* ove_loader_test_exec_arm[], _len */

/* Destination for the loaded module. NuttX flat build: RAM is executable. */
static uint8_t s_region[8192] __attribute__((aligned(8)));

/* Import referenced by the loaded module. Lives in the firmware, far (>16 MB)
 * from the load region, so the BL to it requires a veneer. */
static int host_add(int a, int b)
{
	return a + b;
}

static void test_loader_target_exec(void **state)
{
	(void)state;
	ove_loader_sym_t imports[] = {{"host_add", (void *)host_add}};
	ove_module_t mod;
	assert_int_equal(ove_loader_load(&mod, ove_loader_test_exec_arm,
					 ove_loader_test_exec_arm_len, s_region, sizeof(s_region),
					 imports, 1),
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

	/* R_ARM_ABS32 (literal pool): the loaded code reads its own global. */
	int (*m_read_global)(void) = (int (*)(void))ove_loader_sym(&mod, "m_read_global");
	assert_non_null(m_read_global);
	assert_int_equal(m_read_global(), 0x1234);

	/* R_ARM_THM_CALL via veneer: the loaded code calls the far import. */
	int (*m_use_import)(int) = (int (*)(int))ove_loader_sym(&mod, "m_use_import");
	assert_non_null(m_use_import);
	assert_int_equal(m_use_import(10), 111); /* host_add(10, 100) + 1 */
}

int test_loader_target_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_loader_target_exec),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
