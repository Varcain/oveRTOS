/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * ARM semihosting exit — tells QEMU to shut down with an exit code.
 *
 * Uses SYS_EXIT_EXTENDED (0x20) with ADP_Stopped_ApplicationExit (0x20026).
 * Only works on ARM targets with semihosting enabled (bkpt #0xab).
 */

#ifndef SEMIHOSTING_EXIT_H
#define SEMIHOSTING_EXIT_H

#include <stdint.h>

static inline void semihosting_exit(int code)
{
	const uint32_t args[2] = {0x20026, (uint32_t)code};
	register uint32_t r0 __asm__("r0") = 0x20; /* SYS_EXIT_EXTENDED */
	register const uint32_t *r1 __asm__("r1") = args;
	__asm__ volatile("bkpt #0xab" : : "r"(r0), "r"(r1) : "memory");
	__builtin_unreachable();
}

#endif /* SEMIHOSTING_EXIT_H */
