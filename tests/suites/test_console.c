/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_console_init(void **state)
{
	(void)state;
	int rc = ove_console_init();
	assert_int_equal(rc, OVE_OK);
}

static void test_console_put_char(void **state)
{
	(void)state;
	ove_console_init();
	ove_console_putchar('A');
	ove_console_putchar('\n');
}

static void test_console_write(void **state)
{
	(void)state;
	ove_console_init();
	const char *msg = "Hello, console!\n";
	ove_console_write(msg, strlen(msg));
}

static void test_console_getchar_from_empty_stdin(void **state)
{
	(void)state;
	ove_console_init();
#if !defined(OVE_QEMU_ARM)
	/* Save original stdin fd, redirect to /dev/null, then restore */
	int saved_stdin = dup(STDIN_FILENO);
	int devnull = open("/dev/null", 0 /* O_RDONLY */);
	dup2(devnull, STDIN_FILENO);
	close(devnull);

	int ch = ove_console_getchar();
	/* /dev/null is EOF immediately — getchar must return EOF. */
	assert_int_equal(ch, EOF);

	/* Restore original stdin */
	dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdin);
#else
	skip();
#endif
}

static void test_console_try_getchar_from_empty_stdin(void **state)
{
	(void)state;
	ove_console_init();
#if !defined(OVE_QEMU_ARM)
	int saved_stdin = dup(STDIN_FILENO);
	int devnull = open("/dev/null", O_RDONLY);
	dup2(devnull, STDIN_FILENO);
	close(devnull);

	assert_int_equal(ove_console_try_getchar(), -1);

	dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdin);
#else
	skip();
#endif
}

static void test_console_init_multiple(void **state)
{
	(void)state;
	int rc;
	rc = ove_console_init();
	assert_int_equal(rc, OVE_OK);
	rc = ove_console_init();
	assert_int_equal(rc, OVE_OK);
	rc = ove_console_init();
	assert_int_equal(rc, OVE_OK);
}

static void test_console_ready_subscription_reports_backend_capability(void **state)
{
	(void)state;
	/* The POSIX test backend has no background RX publisher. Polling remains
	 * available and the unsupported subscription is explicit. */
	assert_int_equal(ove_console_set_ready_callback(NULL, NULL), OVE_ERR_NOT_SUPPORTED);
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_console_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_console_init),
		cmocka_unit_test(test_console_put_char),
		cmocka_unit_test(test_console_write),
		cmocka_unit_test(test_console_getchar_from_empty_stdin),
		cmocka_unit_test(test_console_try_getchar_from_empty_stdin),
		cmocka_unit_test(test_console_init_multiple),
		cmocka_unit_test(test_console_ready_subscription_reports_backend_capability),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
