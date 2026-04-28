/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "framework/ove_test.h"
#include <stdio.h>

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void)
{
}

int main(void)
{
	int failures = 0;

	/*
	 * NuttX sim runs on host POSIX — filesystem suite is supported here.
	 */
#define OVE_SUITE(name, label)               \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#define OVE_SUITE_FS(name, label) OVE_SUITE(name, label)
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	return failures ? 1 : 0;
}
