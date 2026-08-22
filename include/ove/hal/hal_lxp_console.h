/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Physical console transport used by the oveRTOS-to-LXP adapter.
 */
#ifndef OVE_HAL_LXP_CONSOLE_H
#define OVE_HAL_LXP_CONSOLE_H

#include <stddef.h>

#include "ove/console.h"

#ifdef __cplusplus
extern "C" {
#endif

void ove_hal_lxp_console_init(void);
int ove_hal_lxp_console_try_getchar(void);
void ove_hal_lxp_console_putchar(int c);
void ove_hal_lxp_console_guest_write(const char *buf, size_t len);

/** Return nonzero when the transport can publish RX-readiness events. */
int ove_hal_lxp_console_ready_events(void);
int ove_hal_lxp_console_set_ready_callback(ove_console_ready_fn callback, const void *context);

#ifdef __cplusplus
}
#endif

#endif /* OVE_HAL_LXP_CONSOLE_H */
