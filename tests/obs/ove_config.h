/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Minimal ove_config.h for the observability ring tests (tests/obs/).
 * Kept separate from tests/ove_config.h so the rings build in isolation,
 * with a small capacity that makes overflow easy to exercise.
 */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_TRACE_STREAM 1
#define CONFIG_OVE_TRACE_RING_SIZE 4096

#endif /* OVE_CONFIG_H */
