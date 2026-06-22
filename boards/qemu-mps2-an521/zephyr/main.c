/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * QEMU MPS2-AN521 (Cortex-M33) Zephyr entry point. This board hosts the Linux
 * personality, whose apps drive their own console (semihosting) and exit via
 * semihosting, so the entry simply hands off to the application's ove_main().
 */

#include "ove/app.h"

int main(void)
{
	ove_app_run();
	return 0;
}
