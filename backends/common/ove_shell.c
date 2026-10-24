/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * The command shell, shared by every backend: line editing, tokenizing and
 * dispatch over ove_console and OVE_LOG, with no RTOS-specific code.
 *
 * One recursive mutex covers the command table, the console line and the
 * running command, so threads can register and dispatch at once; a handler
 * runs with it held, so it may register commands or dispatch a line itself.
 * Each dispatch tokenizes its own copy of the line on the stack, so a nested
 * one leaves the outer command's argv alone.  ove_shell_log_tee() runs inside
 * logging on any thread and must not take the mutex (a handler logs while
 * holding it), so the state it reads is atomic.
 */

#include "ove/shell.h"
#include "ove/console.h"
#include "ove/log.h"
#include "ove/sync.h"
#include "ove/thread.h"
#include <stdint.h>
#include <string.h>

#define SHELL_LINE_MAX 128
#define SHELL_CMD_MAX 16

/* The counters and flags are bytes: the F746 test images have no RAM to
 * spare for the lock without them. */
static struct ove_shell_cmd cmd_table[SHELL_CMD_MAX];
static uint8_t cmd_count;
static char line_buf[SHELL_LINE_MAX];
static uint8_t line_pos;

static ove_shell_output_hook_t s_output_hook;
/* Set while a command handler runs; its log lines also go to the hook. */
static ove_thread_t s_exec_thread;
static uint8_t s_executing;
static uint8_t s_in_hook;

static ove_mutex_t s_lock;
#ifndef OVE_HEAP_SYNC
static ove_mutex_storage_t s_lock_storage;
#endif

enum { LOCK_NONE, LOCK_CREATING, LOCK_READY };
static uint8_t s_lock_state;

/* Created by the first call that needs it, whichever thread that is (not
 * a constructor: Zephyr builds without STATIC_INIT_GNU cannot have one).  A
 * thread that finds another creating it waits a tick at a time, as a yield
 * would never let a lower-priority creator run, then retries if that
 * creation failed. */
static int shell_lock(void)
{
	for (;;) {
		uint8_t state = __atomic_load_n(&s_lock_state, __ATOMIC_ACQUIRE);
		uint8_t expected = LOCK_NONE;

		if (state == LOCK_READY)
			break;
		if (state == LOCK_CREATING) {
			ove_thread_sleep_ms(1);
			continue;
		}
		if (!__atomic_compare_exchange_n(&s_lock_state, &expected, LOCK_CREATING, 0,
						 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			continue;
#ifdef OVE_HEAP_SYNC
		/* Heap builds allocate it, so they spend no static RAM on it. */
		int rc = ove_recursive_mutex_create(&s_lock);
#else
		int rc = ove_recursive_mutex_init(&s_lock, &s_lock_storage);
#endif
		__atomic_store_n(&s_lock_state, rc == OVE_OK ? LOCK_READY : LOCK_NONE,
				 __ATOMIC_RELEASE);
		if (rc != OVE_OK)
			return rc;
	}
	return ove_recursive_mutex_lock(s_lock, OVE_WAIT_FOREVER);
}

static void shell_unlock(void)
{
	ove_recursive_mutex_unlock(s_lock);
}

static void shell_help(int argc, const char *argv[])
{
	unsigned int i;
	(void)argc;
	(void)argv;

	OVE_LOG_INF("Commands:");
	for (i = 0; i < cmd_count; i++) {
		OVE_LOG_INF("  %-12s %s", cmd_table[i].name,
			    cmd_table[i].help ? cmd_table[i].help : "");
	}
}

static int is_blank(char c)
{
	return c == ' ' || c == '\t';
}

/* Called with the lock held; a nested dispatch on the same thread restores
 * the outer command's state when it returns. */
static void run_handler(const struct ove_shell_cmd *cmd, int argc, const char *argv[])
{
	uint8_t was_executing = __atomic_load_n(&s_executing, __ATOMIC_RELAXED);

	__atomic_store_n(&s_exec_thread, ove_thread_get_self(), __ATOMIC_RELAXED);
	__atomic_store_n(&s_executing, 1, __ATOMIC_RELEASE);
	cmd->handler(argc, argv);
	__atomic_store_n(&s_executing, was_executing, __ATOMIC_RELEASE);
}

/* Called with the lock held; tokenizes @p line in place. */
static void shell_execute(char *line)
{
	const char *argv[OVE_SHELL_MAX_ARGS + 1]; /* + the NULL after the last word */
	int argc = 0;
	char *p = line;
	unsigned int i;

	while (*p != '\0' && argc < OVE_SHELL_MAX_ARGS) {
		while (is_blank(*p)) {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		argv[argc++] = p;
		while (*p != '\0' && !is_blank(*p)) {
			p++;
		}
		if (*p != '\0') {
			*p++ = '\0';
		}
	}

	if (argc == 0) {
		return;
	}
	argv[argc] = NULL;

	for (i = 0; i < cmd_count; i++) {
		if (strcmp(argv[0], cmd_table[i].name) == 0) {
			run_handler(&cmd_table[i], argc, argv);
			return;
		}
	}

	OVE_LOG_WRN("Unknown command: %s", argv[0]);
}

/* Called with the lock held. */
static void shell_dispatch(const char *src, size_t len)
{
	char line[SHELL_LINE_MAX];

	if (len >= sizeof(line))
		len = sizeof(line) - 1;
	memcpy(line, src, len);
	line[len] = '\0';
	shell_execute(line);
}

static int register_locked(const struct ove_shell_cmd *cmd)
{
	unsigned int i;

	/* Overwrite existing command with the same name */
	for (i = 0; i < cmd_count; i++) {
		if (strcmp(cmd_table[i].name, cmd->name) == 0) {
			cmd_table[i] = *cmd;
			return OVE_OK;
		}
	}

	if (cmd_count >= SHELL_CMD_MAX) {
		return OVE_ERR_NO_MEMORY;
	}
	cmd_table[cmd_count++] = *cmd;
	return OVE_OK;
}

int ove_shell_init(void)
{
	static const struct ove_shell_cmd help_cmd = {
		.name = "help",
		.help = "List available commands",
		.handler = shell_help,
	};

	int rc = shell_lock();
	if (rc != OVE_OK)
		return rc;
	cmd_count = 0;
	line_pos = 0;
	rc = register_locked(&help_cmd);
	shell_unlock();
	return rc;
}

int ove_shell_register_cmd(const struct ove_shell_cmd *cmd)
{
	if (cmd == NULL || cmd->name == NULL || cmd->handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	int rc = shell_lock();
	if (rc != OVE_OK)
		return rc;
	rc = register_locked(cmd);
	shell_unlock();
	return rc;
}

void ove_shell_process_char(int c)
{
	if (shell_lock() != OVE_OK)
		return;

	if (c == '\n' || c == '\r') {
		size_t len = line_pos;

		/* The text path: unlike putchar it ends the line as the console
		 * needs (CRLF on a serial port). */
		ove_console_write("\n", 1);
		line_pos = 0;
		shell_dispatch(line_buf, len);
		ove_console_write("> ", 2);
	} else if (c == '\b' || c == 127) {
		if (line_pos > 0) {
			line_pos--;
			ove_console_putchar('\b');
			ove_console_putchar(' ');
			ove_console_putchar('\b');
		}
	} else if (line_pos < SHELL_LINE_MAX - 1) {
		line_buf[line_pos++] = (char)c;
		ove_console_putchar(c);
	}

	shell_unlock();
}

void ove_shell_process_line(const char *line)
{
	if (!line || shell_lock() != OVE_OK)
		return;
	shell_dispatch(line, strlen(line));
	shell_unlock();
}

void ove_shell_set_output_hook(ove_shell_output_hook_t hook)
{
	__atomic_store_n(&s_output_hook, hook, __ATOMIC_RELEASE);
}

void ove_shell_log_tee(const char *data, size_t len)
{
	ove_shell_output_hook_t hook = __atomic_load_n(&s_output_hook, __ATOMIC_ACQUIRE);

	/* Only the running command's own lines; a hook that logs must not
	 * feed itself.  s_in_hook is only touched by the running command's
	 * thread, which the thread check selects. */
	if (!hook || !__atomic_load_n(&s_executing, __ATOMIC_ACQUIRE) ||
	    ove_thread_get_self() != __atomic_load_n(&s_exec_thread, __ATOMIC_RELAXED) || s_in_hook)
		return;
	s_in_hook = 1;
	hook(data, len);
	s_in_hook = 0;
}
