/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "ove/protected.h"

#include <string.h>
#include <sys/mman.h>

/* A well-behaved task: writes only into its own buffer. */
static void task_ok(void *arg)
{
	volatile char *buf = (volatile char *)arg;
	for (int i = 0; i < 64; i++)
		buf[i] = (char)i;
}

static void test_ptask_rejects_null(void **state)
{
	(void)state;
	assert_int_equal(ove_ptask_run(NULL, NULL, NULL), OVE_ERR_INVALID_PARAM);
}

static void test_ptask_ok(void **state)
{
	(void)state;
	char buf[64];
	ove_ptask_result_t r = OVE_PTASK_FAULT;
	assert_int_equal(ove_ptask_run(task_ok, buf, &r), OVE_OK);
	assert_int_equal(r, OVE_PTASK_OK);
	assert_int_equal(buf[10], 10); /* the task actually ran */
}

/*
 * The deliberate-fault test is skipped under AddressSanitizer: ASan owns the
 * SIGSEGV handler and reports the intentional wild write as an error. The
 * containment path is exercised in the default (non-sanitized) stub build.
 */
#ifndef __SANITIZE_ADDRESS__
/* A misbehaving task: writes to a region it must not touch. */
static void task_fault(void *arg)
{
	volatile char *forbidden = (volatile char *)arg;
	forbidden[0] = 1; /* PROT_NONE page -> SIGSEGV -> contained */
}

static void test_ptask_fault_contained(void **state)
{
	(void)state;
	void *forbidden = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert_true(forbidden != MAP_FAILED);

	unsigned long before = ove_ptask_fault_count();

	ove_ptask_result_t r = OVE_PTASK_OK;
	assert_int_equal(ove_ptask_run(task_fault, forbidden, &r), OVE_OK);
	assert_int_equal(r, OVE_PTASK_FAULT); /* contained, not crashed */
	assert_int_equal(ove_ptask_fault_count(), before + 1);

	/* The supervisor survived: a subsequent well-behaved run still works. */
	char buf[64];
	ove_ptask_result_t r2 = OVE_PTASK_FAULT;
	assert_int_equal(ove_ptask_run(task_ok, buf, &r2), OVE_OK);
	assert_int_equal(r2, OVE_PTASK_OK);

	munmap(forbidden, 4096);
}
#endif /* !__SANITIZE_ADDRESS__ */

int test_protected_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_ptask_rejects_null),
		cmocka_unit_test(test_ptask_ok),
#ifndef __SANITIZE_ADDRESS__
		cmocka_unit_test(test_ptask_fault_contained),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
