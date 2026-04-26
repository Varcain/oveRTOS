/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static volatile int s_cmd_executed;
static volatile int s_argc_received;
static const char *s_argv_received[8];

static void test_cmd_handler(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    s_cmd_executed = 1;
}

static void test_argv_handler(int argc, const char *argv[])
{
    s_argc_received = argc;
    for (int i = 0; i < argc && i < 8; i++) {
        s_argv_received[i] = argv[i];
    }
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_shell_init(void **state)
{
    (void)state;
    int rc = ove_shell_init();
    assert_int_equal(rc, OVE_OK);
}

static void test_shell_register_cmd(void **state)
{
    (void)state;
    ove_shell_init();

    struct ove_shell_cmd cmd = {
        .name = "help",
        .help = "show help",
        .handler = test_cmd_handler,
    };
    int rc = ove_shell_register_cmd(&cmd);
    assert_int_equal(rc, OVE_OK);
}

static void test_shell_process_char(void **state)
{
    (void)state;
    ove_shell_init();

    struct ove_shell_cmd cmd = {
        .name = "hello",
        .help = "say hello",
        .handler = test_cmd_handler,
    };
    ove_shell_register_cmd(&cmd);

    /* Feed characters — should not crash */
    const char *input = "hel";
    for (size_t i = 0; i < strlen(input); i++) {
        ove_shell_process_char(input[i]);
    }
}

static void test_shell_complete_command(void **state)
{
    (void)state;
    s_cmd_executed = 0;

    ove_shell_init();

    /* Use a non-`help` name — the real freertos_shell backend
     * pre-registers `help` itself, and overriding it isn't part of the
     * API contract.  Stub backends happen to allow re-registration
     * because they start empty, so this collision was previously
     * latent. */
    struct ove_shell_cmd cmd = {
        .name = "ovetestcmd",
        .help = "test command",
        .handler = test_cmd_handler,
    };
    ove_shell_register_cmd(&cmd);

    const char *input = "ovetestcmd\n";
    for (size_t i = 0; i < strlen(input); i++) {
        ove_shell_process_char(input[i]);
    }

    assert_int_equal(s_cmd_executed, 1);
}

static void test_shell_register_null(void **state)
{
    (void)state;
    ove_shell_init();

    int rc = ove_shell_register_cmd(NULL);
    assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_shell_argv_parsing(void **state)
{
    (void)state;
    s_argc_received = 0;
    memset((void *)s_argv_received, 0, sizeof(s_argv_received));

    ove_shell_init();

    struct ove_shell_cmd cmd = {
        .name = "cmd",
        .help = "test argv",
        .handler = test_argv_handler,
    };
    ove_shell_register_cmd(&cmd);

    /* Feed "cmd arg1 arg2\n" */
    const char *input = "cmd arg1 arg2\n";
    for (size_t i = 0; i < strlen(input); i++) {
        ove_shell_process_char(input[i]);
    }

    assert_int_equal(s_argc_received, 3);
    assert_string_equal(s_argv_received[0], "cmd");
    assert_string_equal(s_argv_received[1], "arg1");
    assert_string_equal(s_argv_received[2], "arg2");
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_shell_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_shell_init),
        cmocka_unit_test(test_shell_register_cmd),
        cmocka_unit_test(test_shell_process_char),
        cmocka_unit_test(test_shell_complete_command),
        cmocka_unit_test(test_shell_register_null),
        cmocka_unit_test(test_shell_argv_parsing),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
