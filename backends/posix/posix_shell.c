/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <string.h>
#include <ctype.h>

#define SHELL_MAX_CMDS 32
#define SHELL_LINE_BUF 256

static struct ove_shell_cmd registered_cmds[SHELL_MAX_CMDS];
static int cmd_count;
static char line_buf[SHELL_LINE_BUF];
static int line_pos;

int ove_shell_init(void)
{
	cmd_count = 0;
	line_pos = 0;
	memset(line_buf, 0, sizeof(line_buf));
	return OVE_OK;
}

int ove_shell_register_cmd(const struct ove_shell_cmd *cmd)
{
	if (!cmd || !cmd->name || !cmd->handler) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (cmd_count >= SHELL_MAX_CMDS) {
		return OVE_ERR_NO_MEMORY;
	}
	registered_cmds[cmd_count++] = *cmd;
	return OVE_OK;
}

static void execute_line(void)
{
	if (line_pos == 0) {
		return;
	}
	line_buf[line_pos] = '\0';

	/* Tokenize */
	const char *argv[OVE_SHELL_MAX_ARGS];
	int argc = 0;
	char *p = line_buf;
	while (*p && argc < OVE_SHELL_MAX_ARGS) {
		while (*p && isspace((unsigned char)*p)) {
			p++;
		}
		if (!*p) {
			break;
		}
		argv[argc++] = p;
		while (*p && !isspace((unsigned char)*p)) {
			p++;
		}
		if (*p) {
			*p++ = '\0';
		}
	}

	if (argc == 0) {
		return;
	}

	for (int i = 0; i < cmd_count; i++) {
		if (strcmp(registered_cmds[i].name, argv[0]) == 0) {
			registered_cmds[i].handler(argc, argv);
			return;
		}
	}
}

void ove_shell_process_char(int c)
{
	if (c == '\n' || c == '\r') {
		execute_line();
		line_pos = 0;
	} else if (line_pos < SHELL_LINE_BUF - 1) {
		line_buf[line_pos++] = (char)c;
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
	if (len >= SHELL_LINE_BUF)
		len = SHELL_LINE_BUF - 1;
	memcpy(line_buf, line, len);
	line_pos = (int)len;
	execute_line();
	line_pos = 0;
}
