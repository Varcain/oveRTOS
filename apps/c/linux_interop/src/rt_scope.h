/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LINUX_INTEROP_RT_SCOPE_H
#define LINUX_INTEROP_RT_SCOPE_H

typedef void (*linux_rt_scope_write_fn)(const char *text);

/* Start the STM32F746G-DISCO two-channel host real-time demonstration. */
int linux_rt_scope_start(linux_rt_scope_write_fn write_fn);

#endif /* LINUX_INTEROP_RT_SCOPE_H */
