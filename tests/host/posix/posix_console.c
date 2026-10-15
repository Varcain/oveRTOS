/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int ove_console_init(void)
{
	return OVE_OK;
}

int ove_console_getchar(void)
{
	return getchar();
}

int ove_console_try_getchar(void)
{
	struct pollfd pfd = {
		.fd = STDIN_FILENO,
		.events = POLLIN,
	};
	unsigned char c;
	if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & (POLLIN | POLLHUP)))
		return -1;
	return read(STDIN_FILENO, &c, 1) == 1 ? (int)c : -1;
}

void ove_console_putchar(int c)
{
	putchar(c);
}

void ove_console_write(const char *buf, unsigned int len)
{
	write(STDOUT_FILENO, buf, len);
}

int ove_console_set_ready_callback(ove_console_ready_fn callback, const void *context)
{
	(void)callback;
	(void)context;
	return OVE_ERR_NOT_SUPPORTED;
}
