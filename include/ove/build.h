/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_BUILD_H
#define OVE_BUILD_H

/* Generated for every normal `ove build`. Direct CMake consumers still compile,
 * but say "unknown" instead of embedding a stale or invented source revision. */
#if defined(__has_include)
#if __has_include("ove_build_id.h")
#include "ove_build_id.h"
#endif
#endif

#ifndef OVE_BUILD_ID
#define OVE_BUILD_ID "unknown (built outside 'ove build')"
#endif
#ifndef OVE_BUILD_OVERTOS_REV
#define OVE_BUILD_OVERTOS_REV "unknown"
#endif
#ifndef OVE_BUILD_LXP_REV
#define OVE_BUILD_LXP_REV "unknown"
#endif

#endif /* OVE_BUILD_H */
