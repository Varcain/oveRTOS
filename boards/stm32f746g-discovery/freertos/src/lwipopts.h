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

/* STM32 ETH supports hardware checksum offload for IP/TCP/UDP/ICMP via
 * the per-descriptor CIC=11 (CHECKSUMTCPUDPICMPFULL) default that
 * HAL_ETH_Init programs into every Tx descriptor. Disable software
 * checksums to save CPU and — critically for ICMP — to avoid lwIP's
 * in-place checksum *adjustment* (icmp_input modifies the inbound
 * pbuf's type byte from ECHO to ECHO_REPLY and offsets the checksum).
 * The MAC then recomputes from a packet that no longer has the
 * canonical zero-checksum form and writes a wrong value, which the
 * peer drops as IcmpInCsumErrors. With CHECKSUM_GEN_ICMP=0 lwIP zeros
 * the field and the MAC inserts a correct checksum from scratch. */
#define CHECKSUM_GEN_IP 0
#define CHECKSUM_GEN_UDP 0
#define CHECKSUM_GEN_TCP 0
#define CHECKSUM_GEN_ICMP 0
#define CHECKSUM_CHECK_IP 0
#define CHECKSUM_CHECK_UDP 0
#define CHECKSUM_CHECK_TCP 0
#define CHECKSUM_CHECK_ICMP 0
#define CHECKSUM_BY_HARDWARE 1

/* lwIP's defaults already enable LWIP_ICMP and LWIP_RAW; explicit for
 * clarity since this board exposes ping for diagnostics. */
#define LWIP_ICMP 1
#define LWIP_RAW 1

#include "lwipopts_common.h"

#endif /* LWIPOPTS_H */
