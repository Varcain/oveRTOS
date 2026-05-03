/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* QEMU MPS2-AN500 has no NIC checksum offload — fall back to lwIP
 * software checksums (the common defaults).  Memory sizing matches
 * the upstream defaults, so no overrides beyond the include. */

#include "lwipopts_common.h"

#endif /* LWIPOPTS_H */
