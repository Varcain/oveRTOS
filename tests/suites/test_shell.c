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
/* Copies: argv is valid only during the call.  Same size as the eight
 * pointers it replaced, for the F746 test images' RAM. */
static char s_argv_received[4][8];
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
	for (int i = 0; i < argc && i < 4; i++) {
		strncpy(s_argv_received[i], argv[i], sizeof(s_argv_received[i]) - 1);
		s_argv_received[i][sizeof(s_argv_received[i]) - 1] = '\0';
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

static void count_hook(const char *data, size_t len)
{
	(void)data;
	(void)len;
	s_first_calls++;
}

static void set_hook_handler(int argc, const char *argv[])
{
	(void)argc;
	(void)argv;
	ove_shell_set_output_hook(capture_hook);
}

/* A line run with its own hook: only that hook gets the command's output,
 * the global one is back afterwards, NULL means none, and a hook the
 * command sets itself stays. */
static void test_shell_process_line_with_hook(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	static const struct ove_shell_cmd out = {
		.name = "oveout",
		.help = "emit output",
		.handler = test_output_handler,
	};
	static const struct ove_shell_cmd sethook = {
		.name = "ovesethook",
		.help = "set the global hook",
		.handler = set_hook_handler,
	};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&out));
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&sethook));

	reset_hook_capture();
	s_first_calls = 0;
	ove_shell_set_output_hook(capture_hook);
	ove_shell_process_line_with_hook("oveout mine", count_hook);
	assert_int_equal(s_first_calls, 1);
	assert_int_equal(s_hook_calls, 0);

	ove_shell_process_line("oveout global");
	assert_int_equal(s_hook_calls, 1);
	assert_string_equal(s_hook_buf, "global");

	ove_shell_process_line_with_hook("oveout none", NULL);
	assert_int_equal(s_first_calls, 1);
	assert_int_equal(s_hook_calls, 1);

	ove_shell_set_output_hook(NULL);
	ove_shell_process_line_with_hook("ovesethook", count_hook);
	ove_shell_process_line("oveout kept");
	assert_int_equal(s_hook_calls, 2);

	ove_shell_set_output_hook(NULL);
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

/* ── locking and nested dispatch ─────────────────────────────────────── */

static void nested_inner(int argc, const char *argv[])
{
	(void)argc;
	(void)argv;
	s_first_calls++;
}

/* Registers a command and dispatches it from inside a command: the shell's
 * lock is held across the handler, so it must be recursive, and the nested
 * line must not overwrite the line the outer argv points into (it used the
 * same buffer, so "hello" became "inner"). */
static void nested_outer(int argc, const char *argv[])
{
	static const struct ove_shell_cmd inner = {
		.name = "inner",
		.help = "nested",
		.handler = nested_inner,
	};
	if (ove_shell_register_cmd(&inner) != OVE_OK || argc < 2)
		return;
	ove_shell_process_line("inner XXXXXXXX YYYYYYYY");
	s_second_calls = strcmp(argv[1], "hello") == 0;
}

static void test_shell_nested_dispatch(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	static const struct ove_shell_cmd outer = {
		.name = "outer",
		.help = "nests",
		.handler = nested_outer,
	};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&outer));
	s_first_calls = 0;
	s_second_calls = 0;

	ove_shell_process_line("outer hello");
	assert_int_equal(s_first_calls, 1);
	assert_int_equal(s_second_calls, 1);
}

#ifdef CONFIG_OVE_RTOS_POSIX
/* Two threads using the shell at once.  Host only: these need a thread
 * stack the F746 test images have no RAM for, and TSan (stub-tsan) is what
 * catches an unlocked table or line here. */

OVE_TEST_STORAGE(ove_thread_storage_t, s_race_th_storage);
OVE_TEST_STACK(s_race_th_stack, 4096);

#define RACE_ITERS 2000

static int s_race_done;
static int s_race_reg_errs;
static int s_race_main_calls;
static int s_race_other_calls;
static ove_thread_t s_race_main;

static void race_handler(int argc, const char *argv[])
{
	(void)argc;
	(void)argv;
}

static const char *const s_race_fnames[] = {
	"f00", "f01", "f02", "f03", "f04", "f05", "f06",
	"f07", "f08", "f09", "f10", "f11", "f12", "f13",
};

static void race_register(const struct ove_shell_cmd *cmd)
{
	if (ove_shell_register_cmd(cmd) != OVE_OK)
		__atomic_fetch_add(&s_race_reg_errs, 1, __ATOMIC_RELAXED);
}

static void race_registrar_entry(void *arg)
{
	(void)arg;
	for (int i = 0; i < RACE_ITERS; i++) {
		struct ove_shell_cmd c = {.name = "race", .help = "race", .handler = race_handler};
		race_register(&c);
		if ((i % 8) == 0) {
			if (ove_shell_init() != OVE_OK)
				__atomic_fetch_add(&s_race_reg_errs, 1, __ATOMIC_RELAXED);
			for (size_t k = 0; k < sizeof(s_race_fnames) / sizeof(s_race_fnames[0]);
			     k++) {
				struct ove_shell_cmd f = {.name = s_race_fnames[k],
							  .help = "fill",
							  .handler = race_handler};
				race_register(&f);
			}
		}
	}
	__atomic_store_n(&s_race_done, 1, __ATOMIC_RELEASE);
}

/* Registering and re-initialising on one thread while another dispatches. */
static void test_shell_concurrent_register_dispatch(void **state)
{
	(void)state;
	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	__atomic_store_n(&s_race_done, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s_race_reg_errs, 0, __ATOMIC_RELAXED);

	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_race_th_storage, "shreg",
					       race_registrar_entry, NULL, s_race_th_stack, 4096));
	while (!__atomic_load_n(&s_race_done, __ATOMIC_ACQUIRE)) {
		ove_shell_process_line("race");
		ove_shell_process_line("f07");
	}
	ove_test_thread_destroy(th);
	assert_int_equal(__atomic_load_n(&s_race_reg_errs, __ATOMIC_RELAXED), 0);
}

static void help_counter(int argc, const char *argv[])
{
	(void)argc;
	(void)argv;
	if (ove_thread_get_self() == s_race_main)
		__atomic_fetch_add(&s_race_main_calls, 1, __ATOMIC_RELAXED);
	else
		__atomic_fetch_add(&s_race_other_calls, 1, __ATOMIC_RELAXED);
}

static void race_feeder_entry(void *arg)
{
	(void)arg;
	for (int i = 0; !__atomic_load_n(&s_race_done, __ATOMIC_ACQUIRE); i++) {
		ove_shell_process_char('h');
		ove_shell_process_char('e');
		if ((i % 32) == 31)
			ove_shell_process_char('\n');
	}
}

/* Typing on one thread while another dispatches whole lines: process_line
 * shared the typed line's buffer, so lines were lost and the typist ran the
 * other thread's command. */
static void test_shell_process_line_beside_typing(void **state)
{
	(void)state;
	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_shell_init());
	static const struct ove_shell_cmd h = {
		.name = "help", .help = "count", .handler = help_counter};
	OVE_TEST_ASSERT_OK(ove_shell_register_cmd(&h));
	s_race_main = ove_thread_get_self();
	__atomic_store_n(&s_race_done, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s_race_main_calls, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s_race_other_calls, 0, __ATOMIC_RELAXED);

	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_race_th_storage, "shfeed", race_feeder_entry,
					       NULL, s_race_th_stack, 4096));
	for (int i = 0; i < RACE_ITERS; i++)
		ove_shell_process_line("help");
	__atomic_store_n(&s_race_done, 1, __ATOMIC_RELEASE);
	ove_test_thread_destroy(th);
	ove_shell_process_char('\n'); /* drop the half-typed line */

	assert_int_equal(__atomic_load_n(&s_race_main_calls, __ATOMIC_RELAXED), RACE_ITERS);
	assert_int_equal(__atomic_load_n(&s_race_other_calls, __ATOMIC_RELAXED), 0);
}
#endif /* CONFIG_OVE_RTOS_POSIX */

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
		cmocka_unit_test(test_shell_process_line_with_hook),
		cmocka_unit_test(test_shell_process_line_terminates),
		cmocka_unit_test(test_shell_reregister_replaces),
		cmocka_unit_test(test_shell_register_copies_descriptor),
		cmocka_unit_test(test_shell_reregister_when_full),
		cmocka_unit_test(test_shell_nested_dispatch),
#ifdef CONFIG_OVE_RTOS_POSIX
		cmocka_unit_test(test_shell_concurrent_register_dispatch),
		cmocka_unit_test(test_shell_process_line_beside_typing),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
