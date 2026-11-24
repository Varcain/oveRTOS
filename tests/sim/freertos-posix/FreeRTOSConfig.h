/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_FREERTOS_POSIX_CONFIG_H
#define OVE_FREERTOS_POSIX_CONFIG_H

#include "../freertos-qemu/FreeRTOSConfig.h"

/* Each task gets libc TLS from its pthread. */
#undef configUSE_PICOLIBC_TLS

/* Keep 1 KiB for the FreeRTOS stack that holds the port's thread record. */
#undef configMINIMAL_STACK_SIZE
#define configMINIMAL_STACK_SIZE ((configSTACK_DEPTH_TYPE)(1024 / sizeof(StackType_t)))

/* Use the kernel's default static idle and timer task storage. */
#define configKERNEL_PROVIDED_STATIC_MEMORY 1

#endif /* OVE_FREERTOS_POSIX_CONFIG_H */
