/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Sim console backend — replaces posix_console.c when CONFIG_OVE_SIM=y.
 *
 * Console output goes to stdout and is buffered for the dashboard.
 * A complete line (or a full buffer) is broadcast as a single WS frame.
 * Console input comes from both stdin and the dashboard WebSocket
 * (via a pipe written by the WS server).
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>

extern void ove_sim_log_broadcast(const char *msg, unsigned int len);

/* ── Dashboard input pipe ─────────────────────────────────────────── */

static int ws_pipe_rd = -1;
static int ws_pipe_wr = -1;

int ove_sim_console_pipe_fd(void)
{
	return ws_pipe_wr;
}

/* ── Output line buffer ───────────────────────────────────────────── */

#define LINE_BUF_SIZE 512

static char line_buf[LINE_BUF_SIZE];
static unsigned int line_pos;
static pthread_mutex_t line_lock = PTHREAD_MUTEX_INITIALIZER;

static void flush_line_buf(void)
{
	if (line_pos > 0) {
		ove_sim_log_broadcast(line_buf, line_pos);
		line_pos = 0;
	}
}

static void line_buf_append(const char *data, unsigned int len)
{
	pthread_mutex_lock(&line_lock);
	for (unsigned int i = 0; i < len; i++) {
		line_buf[line_pos++] = data[i];
		if (data[i] == '\n' || line_pos >= LINE_BUF_SIZE)
			flush_line_buf();
	}
	pthread_mutex_unlock(&line_lock);
}

/* ── Public API ───────────────────────────────────────────────────── */

int ove_console_init(void)
{
	int fds[2];
	if (pipe(fds) == 0) {
		ws_pipe_rd = fds[0];
		ws_pipe_wr = fds[1];
		fcntl(ws_pipe_rd, F_SETFL, fcntl(ws_pipe_rd, F_GETFL) | O_NONBLOCK);
	}
	return OVE_OK;
}

int ove_console_getchar(void)
{
	struct pollfd pfd[2];
	int nfds = 0;

	pfd[nfds].fd = STDIN_FILENO;
	pfd[nfds].events = POLLIN;
	nfds++;

	if (ws_pipe_rd >= 0) {
		pfd[nfds].fd = ws_pipe_rd;
		pfd[nfds].events = POLLIN;
		nfds++;
	}

	if (poll(pfd, (nfds_t)nfds, -1) <= 0)
		return -1;

	for (int i = 0; i < nfds; i++) {
		if (pfd[i].revents & POLLIN) {
			unsigned char c;
			if (read(pfd[i].fd, &c, 1) == 1)
				return (int)c;
		}
	}
	return -1;
}

void ove_console_putchar(int c)
{
	putchar(c);
	char ch = (char)c;
	line_buf_append(&ch, 1);
}

void ove_console_write(const char *buf, unsigned int len)
{
	write(STDOUT_FILENO, buf, len);
	line_buf_append(buf, len);
}
