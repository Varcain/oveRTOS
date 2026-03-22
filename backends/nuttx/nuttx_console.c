/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/console.h"
#include "ove_backend_common.h"
#include <stdio.h>

int ove_console_init(void)
{
	return OVE_OK;
}

int ove_console_getchar(void)
{
	return getchar();
}

void ove_console_putchar(int c)
{
	putchar(c);
}

void ove_console_write(const char *buf, unsigned int len)
{
	printf("%.*s", (int)len, buf);
	fflush(stdout);
}
