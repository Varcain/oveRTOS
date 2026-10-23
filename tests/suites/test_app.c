/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

#include <stdio.h>

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

/* ove/version.h: the string spells the numbers, and the packed form orders
 * versions the way they compare. */
static void test_app_version_is_consistent(void **state)
{
	(void)state;
	char spelled[16];
	(void)snprintf(spelled, sizeof(spelled), "%d.%d.%d", OVE_VERSION_MAJOR, OVE_VERSION_MINOR,
		       OVE_VERSION_PATCH);
	assert_string_equal(spelled, OVE_VERSION_STRING);
	assert_int_equal(OVE_VERSION, OVE_VERSION_NUMBER(OVE_VERSION_MAJOR, OVE_VERSION_MINOR,
							 OVE_VERSION_PATCH));
	assert_true(OVE_VERSION_NUMBER(0, 9, 9) < OVE_VERSION_NUMBER(1, 0, 0));
	assert_true(OVE_VERSION_NUMBER(1, 2, 255) < OVE_VERSION_NUMBER(1, 3, 0));
#if OVE_VERSION < OVE_VERSION_NUMBER(0, 1, 0)
	fail_msg("OVE_VERSION below 0.1.0, the first release"); /* and usable in #if */
#endif
}

/*
 * ove_run() starts the scheduler which blocks forever on POSIX
 * (pthread_join). We can't call it in a test — successful linkage of
 * this translation unit is checked at build time, so no runtime
 * tautology test is needed.
 */

int test_app_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_app_run_returns_ok),
		cmocka_unit_test(test_app_version_is_consistent),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
