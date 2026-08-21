/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LINUX_INTEROP_ROUNDTRIP_H
#define LINUX_INTEROP_ROUNDTRIP_H

#include "ove/lxp_launch.h"

/* Start the native worker and bind its allocation-free guest I/O callbacks. */
int linux_interop_roundtrip_prepare(ove_lxp_launch_config_t *config);

/* Stop the worker and verify the completed RTOS -> guest -> RTOS exchange. */
int linux_interop_roundtrip_complete(int guest_status);

#endif /* LINUX_INTEROP_ROUNDTRIP_H */
