/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * On-target protected-task test: exercise MPU-backed fault containment on the
 * Cortex-M. A well-behaved entry runs to completion; an entry that writes to
 * the MPU guard region traps into MemManage, is contained, and the supervisor
 * survives to run again. The Cortex-M analog of the host backend's PROT_NONE
 * containment test. Lives outside tests/suites/ (not a host-categorised suite)
 * and is dispatched only from the NuttX runner under CONFIG_OVE_PROTECTED.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_PROTECTED)

#include "framework/ove_test.h"
#include "ove/protected.h"
#include "ove_protected_nuttx.h"

#include <stddef.h>

/* A well-behaved task: writes only into the supervisor-provided cell. */
static void task_ok(void *arg)
{
	volatile int *cell = (volatile int *)arg;
	*cell = 0x600d;
}

/* A misbehaving task: writes into the MPU no-access guard region. */
static void task_fault(void *arg)
{
	volatile unsigned char *forbidden = (volatile unsigned char *)arg;
	forbidden[0] = 0xff; /* MemManage -> contained */
}

static void test_ptask_rejects_null(void **state)
{
	(void)state;
	assert_int_equal(ove_ptask_run(NULL, NULL, NULL), OVE_ERR_INVALID_PARAM);
}

static void test_ptask_ok(void **state)
{
	(void)state;
	int cell = 0;
	ove_ptask_result_t r = OVE_PTASK_FAULT;
	assert_int_equal(ove_ptask_run(task_ok, &cell, &r), OVE_OK);
	assert_int_equal(r, OVE_PTASK_OK);
	assert_int_equal(cell, 0x600d); /* the task actually ran */
}

static void test_ptask_fault_contained(void **state)
{
	(void)state;
	void *forbidden = (void *)ove_nuttx_ptask_guarded_region(NULL);
	assert_non_null(forbidden);

	unsigned long before = ove_ptask_fault_count();

	ove_ptask_result_t r = OVE_PTASK_OK;
	assert_int_equal(ove_ptask_run(task_fault, forbidden, &r), OVE_OK);
	assert_int_equal(r, OVE_PTASK_FAULT); /* contained, not crashed */
	assert_int_equal(ove_ptask_fault_count(), before + 1);

	/* The supervisor survived: a subsequent well-behaved run still works. */
	int cell = 0;
	ove_ptask_result_t r2 = OVE_PTASK_FAULT;
	assert_int_equal(ove_ptask_run(task_ok, &cell, &r2), OVE_OK);
	assert_int_equal(r2, OVE_PTASK_OK);
	assert_int_equal(cell, 0x600d);
}

int test_protected_target_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_ptask_rejects_null),
		cmocka_unit_test(test_ptask_ok),
		cmocka_unit_test(test_ptask_fault_contained),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

#endif /* CONFIG_OVE_PROTECTED */
