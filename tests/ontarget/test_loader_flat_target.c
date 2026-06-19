/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * On-target bFLT (uClinux flat) loader test: load a real elf2flt-produced flat
 * program into RAM and *execute* its entry on the Cortex-M. The program reads a
 * global through its absolute address, so a correct run proves bFLT parsing,
 * header stripping, and base relocation against genuine elf2flt output. Lives
 * outside tests/suites/ (not a host-categorised suite) and is dispatched only
 * from the NuttX runner.
 */

#include "framework/ove_test.h"
#include "ove/loader.h"

#include "loader_flat_mod_image.h" /* ove_loader_test_flat_arm[], _len */

/* Destination for the loaded program. NuttX flat build: RAM is executable. */
static uint8_t s_flat_region[4096] __attribute__((aligned(8)));

static void test_loader_flat_exec(void **state)
{
	(void)state;
	ove_flat_t prog;
	assert_int_equal(ove_loader_load_flat(&prog, ove_loader_test_flat_arm,
					      ove_loader_test_flat_arm_len, s_flat_region,
					      sizeof(s_flat_region)),
			 OVE_OK);

	/* prog.entry is directly callable (carries the Thumb bit). The program
	 * reads a global via its relocated absolute address and returns it + 1. */
	int (*entry)(void) = (int (*)(void))prog.entry;
	assert_int_equal(entry(), 0x600e); /* flat_g(0x600d) + 1 after base reloc */
}

int test_loader_flat_target_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_loader_flat_exec),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
