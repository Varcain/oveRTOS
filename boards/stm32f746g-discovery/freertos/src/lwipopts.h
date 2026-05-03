/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ── Board-specific overrides (must precede the include) ────────── */

/* STM32F746G has 320 KB SRAM, allowing a roomier lwIP heap and
 * larger sys_arch threads. */
#define MEM_SIZE (24 * 1024)
#define TCPIP_THREAD_STACKSIZE 8192
#define DEFAULT_THREAD_STACKSIZE 4096

/* STM32 ETH supports hardware checksum offload — disable software
 * checksums to save CPU. */
#define CHECKSUM_GEN_IP 0
#define CHECKSUM_GEN_UDP 0
#define CHECKSUM_GEN_TCP 0
#define CHECKSUM_CHECK_IP 0
#define CHECKSUM_CHECK_UDP 0
#define CHECKSUM_CHECK_TCP 0
#define CHECKSUM_BY_HARDWARE 1

/* lwIP's defaults already enable LWIP_ICMP and LWIP_RAW; explicit for
 * clarity since this board exposes ping for diagnostics. */
#define LWIP_ICMP 1
#define LWIP_RAW 1

#include "lwipopts_common.h"

#endif /* LWIPOPTS_H */
