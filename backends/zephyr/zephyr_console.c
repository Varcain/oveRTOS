/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/console.h"
#include "ove_backend_common.h"
#include <zephyr/console/console.h>
#include <zephyr/sys/printk.h>

int ove_console_init(void)
{
	console_init();
	return OVE_OK;
}

int ove_console_getchar(void)
{
	return (int)console_getchar();
}

void ove_console_putchar(int c)
{
	printk("%c", (char)c);
}

void ove_console_write(const char *buf, unsigned int len)
{
	printk("%.*s", (int)len, buf);
}
