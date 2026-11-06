/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "checks.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/* Model resets must preserve the process's accumulated diagnostics. */
static atomic_uint failures;

void fixture_check(bool condition, const char *message)
{
	if (!condition) {
		atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed);
		(void)fprintf(stderr, "failed: %s\n", message);
	}
}

_Noreturn void fixture_reject(const char *message)
{
	/* Preserve all live worker backing by ending the process without unwinding. */
	(void)fprintf(stderr, "fixture rejected: %s\n", message);
	(void)fflush(NULL);
	_Exit(2);
}

int fixture_finish(const char *name)
{
	unsigned int count = atomic_load_explicit(&failures, memory_order_relaxed);
	(void)printf("%s: %u failed checks\n", name, count);
	return count ? 1 : 0;
}
