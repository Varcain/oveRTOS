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
    /* Should not crash */
    ove_console_putchar('A');
    ove_console_putchar('\n');
}

static void test_console_write(void **state)
{
    (void)state;
    ove_console_init();
    /* Should not crash */
    const char *msg = "Hello, console!\n";
    ove_console_write(msg, strlen(msg));
}

static void test_console_getchar_no_crash(void **state)
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
    /* Should return EOF (-1) or any value without crashing */
    (void)ch;

    /* Restore original stdin */
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
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

/* ── runner ──────────────────────────────────────────────────────────── */

int test_console_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_console_init),
        cmocka_unit_test(test_console_put_char),
        cmocka_unit_test(test_console_write),
        cmocka_unit_test(test_console_getchar_no_crash),
        cmocka_unit_test(test_console_init_multiple),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
