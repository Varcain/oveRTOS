/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Minimal ove_config.h for the observability ring tests (tests/obs/).
 * Kept separate from tests/ove_config.h so the rings build in isolation.
 * A test can shrink the ring by defining CONFIG_OVE_TRACE_RING_SIZE.
 */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_TRACE_STREAM 1
#ifndef CONFIG_OVE_TRACE_RING_SIZE
#define CONFIG_OVE_TRACE_RING_SIZE 4096
#endif

#endif /* OVE_CONFIG_H */
