/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/console.h"
#include "ove_backend_common.h"
#include "serial_wrapper.h"

int ove_console_init(void)
{
	serial_init();
	return OVE_OK;
}

int ove_console_getchar(void)
{
	return ove_console_try_getchar();
}

int ove_console_try_getchar(void)
{
	return serial_rx_ready() ? (int)serial_getChar() : -1;
}

void ove_console_putchar(int c)
{
	unsigned char ch = (unsigned char)c;
	serial_write(&ch, 1);
}

void ove_console_write(const char *buf, unsigned int len)
{
	serial_write((const unsigned char *)buf, len);
}
