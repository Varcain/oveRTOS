/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stub console backend for bare-metal testing (QEMU).
 * Uses semihosting-provided stdio (printf/getchar work via rdimon.specs).
 */

#include "ove/ove.h"
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
	for (unsigned int i = 0; i < len; i++) {
		putchar(buf[i]);
	}
	fflush(stdout);
}
