/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/shell.h"
#include "ove/log.h"
#include "ove_backend_common.h"
#include <string.h>

#define SHELL_LINE_MAX  128
#define SHELL_CMD_MAX   16

static struct ove_shell_cmd cmd_table[SHELL_CMD_MAX];
static unsigned int cmd_count;
static char line_buf[SHELL_LINE_MAX];
static unsigned int line_pos;

static void shell_help(int argc, const char *argv[])
{
	unsigned int i;
	(void)argc;
	(void)argv;

	OVE_LOG_INF("Commands:\n");
	for (i = 0; i < cmd_count; i++) {
		OVE_LOG_INF("  %-12s %s\n", cmd_table[i].name,
			     cmd_table[i].help ? cmd_table[i].help : "");
	}
}

static void shell_execute(char *line)
{
	const char *argv[OVE_SHELL_MAX_ARGS];
	int argc = 0;
	char *p = line;
	unsigned int i;

	while (*p != '\0' && argc < OVE_SHELL_MAX_ARGS) {
		while (*p == ' ') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		argv[argc++] = p;
		while (*p != '\0' && *p != ' ') {
			p++;
		}
		if (*p != '\0') {
			*p++ = '\0';
		}
	}

	if (argc == 0) {
		return;
	}

	for (i = 0; i < cmd_count; i++) {
		if (strcmp(argv[0], cmd_table[i].name) == 0) {
			cmd_table[i].handler(argc, argv);
			return;
		}
	}

	OVE_LOG_WRN("Unknown command: %s\n", argv[0]);
}

int ove_shell_register_cmd(const struct ove_shell_cmd *cmd);

int ove_shell_init(void)
{
	cmd_count = 0;
	line_pos = 0;

	static const struct ove_shell_cmd help_cmd = {
		.name = "help",
		.help = "List available commands",
		.handler = shell_help,
	};
	return ove_shell_register_cmd(&help_cmd);
}

int ove_shell_register_cmd(const struct ove_shell_cmd *cmd)
{
	unsigned int i;

	if (cmd == NULL || cmd->name == NULL || cmd->handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

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

void ove_shell_process_char(int c)
{
	if (c == '\n' || c == '\r') {
		ove_console_putchar('\n');
		line_buf[line_pos] = '\0';
		shell_execute(line_buf);
		line_pos = 0;
		OVE_LOG_INF("> ");
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
}

static ove_shell_output_hook_t s_output_hook;

void ove_shell_set_output_hook(ove_shell_output_hook_t hook)
{
	s_output_hook = hook;
}

void ove_shell_process_line(const char *line)
{
	if (!line) return;

	/* Copy into the line buffer and execute */
	size_t len = strlen(line);
	if (len >= SHELL_LINE_MAX)
		len = SHELL_LINE_MAX - 1;
	memcpy(line_buf, line, len);
	line_pos = (unsigned int)len;
	shell_execute(line_buf);
	line_pos = 0;
}
