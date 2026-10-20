/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* Minimal ove_config.h for the sim profiler drain test. */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_PROFILER 1
#define CONFIG_OVE_PROFILER_HZ 1000
#define CONFIG_OVE_PROFILER_MAX_DEPTH 8
#define CONFIG_OVE_PROFILER_RING_SIZE 256

#endif /* OVE_CONFIG_H */
