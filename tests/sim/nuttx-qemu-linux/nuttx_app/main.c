/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Minimal isolated runner: executes ONLY the Linux-personality on-target test
 * (load a bFLT, run it under the SVC trap). Kept separate from the full
 * ove_test suite because pulling this test into the 291-test combined run
 * triggers a pre-existing, layout-sensitive memory corruption in that suite
 * (tracked separately); a delicate SVC-interposition test is best validated in
 * a small, dedicated firmware anyway.
 */

#include "framework/semihosting_exit.h"
#include <stdio.h>

int test_linux_target_run(void);

int main(int argc, char *argv[])
{
	int failures = 0;

	(void)argc;
	(void)argv;

	printf("=== Linux personality (isolated on-target bFLT syscall) Tests ===\n");
	failures += test_linux_target_run();

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	semihosting_exit(failures ? 1 : 0);
}
