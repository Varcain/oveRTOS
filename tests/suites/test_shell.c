/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <stdio.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static volatile int s_cmd_executed;
static volatile int s_argc_received;
static const char *s_argv_received[8];
static int s_argv_terminated;

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
	s_argv_terminated = argv[argc] == NULL;
}

static char s_hook_buf[128];
static size_t s_hook_len;
static int s_hook_calls;
static int s_first_calls;
static int s_second_calls;

static void capture_hook(const char *data, size_t len)
{
	size_t room = sizeof(s_hook_buf) - 1 - s_hook_len;
	size_t n = len < room ? len : room;
	memcpy(s_hook_buf + s_hook_len, data, n);
	s_hook_len += n;
	s_hook_buf[s_hook_len] = '\0';
	s_hook_calls++;
	/* A hook that logs must not be fed its own output. */
	ove_shell_log_tee("echo\n", 5);
}

static void reset_hook_capture(void)
{
	memset(s_hook_buf, 0, sizeof(s_hook_buf));
	s_hook_len = 0;
	s_hook_calls = 0;
}

/* Emits a line the way OVE_LOG_* does (the host tests compile the log
 * macros out, so call the tee they use directly). */
static void test_output_handler(int argc, const char *argv[])
{
	(void)argc;
	ove_shell_log_tee(argv[1], strlen(argv[1]));
}

static void test_first_handler(int argc, const char *argv[])
{
	(void)argc;
	(void)argv;
	s_first_calls++;
}

static void test_second_handler(int argc, const char *argv[])
{
	(void)argc;
	(void)argv;
	s_second_calls++;
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
	OVE_TEST_ASSERT_OK(ove_shell_init());

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
	OVE_TEST_ASSERT_OK(ove_shell_init());

	struct ove_shell_cmd cmd = {
		.name = "hello",
		.help = "say hello",
		.handler = test_cmd_handler,
	};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&cmd));

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

	OVE_TEST_ASSERT_OK(ove_shell_init());

	/* Use a non-`help` name — the shell pre-registers `help` itself. */
	struct ove_shell_cmd cmd = {
		.name = "ovetestcmd",
		.help = "test command",
		.handler = test_cmd_handler,
	};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&cmd));

	const char *input = "ovetestcmd\n";
	for (size_t i = 0; i < strlen(input); i++) {
		ove_shell_process_char(input[i]);
	}

	assert_int_equal(s_cmd_executed, 1);
}

static void test_shell_register_null(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_shell_init());

	int rc = ove_shell_register_cmd(NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_shell_argv_parsing(void **state)
{
	(void)state;
	s_argc_received = 0;
	memset((void *)s_argv_received, 0, sizeof(s_argv_received));

	OVE_TEST_ASSERT_OK(ove_shell_init());

	struct ove_shell_cmd cmd = {
		.name = "cmd",
		.help = "test argv",
		.handler = test_argv_handler,
	};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&cmd));

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

static void test_shell_output_hook(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	struct ove_shell_cmd cmd = {
		.name = "oveout",
		.help = "emit output",
		.handler = test_output_handler,
	};
	assert_int_equal(ove_shell_register_cmd(&cmd), OVE_OK);

	reset_hook_capture();
	ove_shell_set_output_hook(capture_hook);
	ove_shell_process_line("oveout hello");
	assert_int_equal(s_hook_calls, 1);
	assert_string_equal(s_hook_buf, "hello");

	/* Output outside a running command is not shell output. */
	ove_shell_log_tee("idle", 4);
	assert_int_equal(s_hook_calls, 1);

	ove_shell_set_output_hook(NULL);
	ove_shell_process_line("oveout again");
	assert_int_equal(s_hook_calls, 1);
}

static void test_shell_process_line_terminates(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	struct ove_shell_cmd cmd = {
		.name = "cmd",
		.help = "test argv",
		.handler = test_argv_handler,
	};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&cmd));

	/* A shorter line after a longer one must not inherit its tail. */
	ove_shell_process_line("cmd aaaa bbbb");
	assert_int_equal(s_argc_received, 3);
	ove_shell_process_line("cmd x");
	assert_int_equal(s_argc_received, 2);
	assert_string_equal(s_argv_received[1], "x");
}

static void test_shell_reregister_replaces(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	struct ove_shell_cmd first = {
		.name = "ovedup",
		.help = "first",
		.handler = test_first_handler,
	};
	struct ove_shell_cmd second = {
		.name = "ovedup",
		.help = "second",
		.handler = test_second_handler,
	};
	s_first_calls = 0;
	s_second_calls = 0;
	assert_int_equal(ove_shell_register_cmd(&first), OVE_OK);
	assert_int_equal(ove_shell_register_cmd(&second), OVE_OK);
	ove_shell_process_line("ovedup");
	assert_int_equal(s_first_calls, 0);
	assert_int_equal(s_second_calls, 1);
}

static void test_shell_argv_null_terminated(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	struct ove_shell_cmd cmd = {
		.name = "cmd",
		.help = "test argv",
		.handler = test_argv_handler,
	};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&cmd));

	s_argv_terminated = 0;
	ove_shell_process_line("cmd a");
	assert_int_equal(s_argc_received, 2);
	assert_true(s_argv_terminated);

	/* A full line: OVE_SHELL_MAX_ARGS words, the NULL still fits. */
	s_argv_terminated = 0;
	ove_shell_process_line("cmd 1 2 3 4 5 6 7 8 9");
	assert_int_equal(s_argc_received, OVE_SHELL_MAX_ARGS);
	assert_true(s_argv_terminated);
}

/* The table keeps a copy of the descriptor: overwriting the caller's struct
 * after registration does not affect the command.  The struct is static so
 * the overwrite cannot be optimised away; an automatic one going out of scope
 * is the same case. */
static void test_shell_register_copies_descriptor(void **state)
{
	(void)state;
	static struct ove_shell_cmd cmd;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	cmd = (struct ove_shell_cmd){
		.name = "ovecopy",
		.help = "copy",
		.handler = test_first_handler,
	};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&cmd));
	memset(&cmd, 0xA5, sizeof(cmd));

	s_first_calls = 0;
	s_second_calls = 0;
	ove_shell_process_line("ovecopy");
	assert_int_equal(s_first_calls, 1);
	assert_int_equal(s_second_calls, 0);
}

/* A full table still accepts re-registering a name already in it. */
static void test_shell_reregister_when_full(void **state)
{
	(void)state;
	static char names[20][8];	      /* more than the table holds */
	OVE_TEST_ASSERT_OK(ove_shell_init()); /* registers "help" */

	int rc = OVE_OK;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && rc == OVE_OK; i++) {
		snprintf(names[i], sizeof(names[i]), "f%02u", (unsigned)i);
		struct ove_shell_cmd c = {
			.name = names[i],
			.help = "fill",
			.handler = test_first_handler,
		};
		rc = ove_shell_register_cmd(&c);
	}
	assert_int_equal(rc, OVE_ERR_NO_MEMORY);

	struct ove_shell_cmd again = {
		.name = "f03",
		.help = "replaced",
		.handler = test_second_handler,
	};
	s_first_calls = 0;
	s_second_calls = 0;
	assert_int_equal(ove_shell_register_cmd(&again), OVE_OK);
	ove_shell_process_line("f03");
	assert_int_equal(s_first_calls, 0);
	assert_int_equal(s_second_calls, 1);
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
		cmocka_unit_test(test_shell_argv_null_terminated),
		cmocka_unit_test(test_shell_output_hook),
		cmocka_unit_test(test_shell_process_line_terminates),
		cmocka_unit_test(test_shell_reregister_replaces),
		cmocka_unit_test(test_shell_register_copies_descriptor),
		cmocka_unit_test(test_shell_reregister_when_full),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
