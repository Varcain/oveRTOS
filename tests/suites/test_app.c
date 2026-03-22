/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

/*
 * ove_main is provided by the test binary's main file (stub_main.c,
 * sim_main.c, etc.). We just verify ove_app_run() returns OK and
 * ove_run() links.
 */

static void test_app_run_returns_ok(void **state)
{
	(void)state;
	int ret = ove_app_run();
	assert_int_equal(ret, OVE_OK);
}

static void test_ove_run_linkable(void **state)
{
	(void)state;
	/*
	 * ove_run() starts the scheduler which blocks forever on POSIX
	 * (pthread_join). We can't call it in a test — just verify it links.
	 */
	void (*fn)(void) = ove_run;
	assert_non_null(fn);
}

int test_app_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_app_run_returns_ok),
		cmocka_unit_test(test_ove_run_linkable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
