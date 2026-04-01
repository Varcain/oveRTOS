/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ── Platform / OS ───────────────────────────────────────────────── */
#define NO_SYS                  0       /* Use OS (FreeRTOS) */
#define LWIP_SOCKET             1       /* BSD socket API */
#define LWIP_NETCONN            1       /* Netconn API (needed by sockets) */
#define LWIP_COMPAT_SOCKETS     0       /* Don't pollute namespace */

/* ── Memory ──────────────────────────────────────────────────────── */
#define MEM_ALIGNMENT           4
#define MEM_SIZE                (16 * 1024)
#define MEMP_NUM_PBUF           16
#define MEMP_NUM_TCP_PCB        16
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define MEMP_NUM_UDP_PCB        8
#define MEMP_NUM_NETCONN        16
#define PBUF_POOL_SIZE          16
#define PBUF_POOL_BUFSIZE       1536

/* ── TCP ─────────────────────────────────────────────────────────── */
#define LWIP_TCP                1
#define TCP_MSS                 1460
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_WND                 (4 * TCP_MSS)

/* ── UDP ─────────────────────────────────────────────────────────── */
#define LWIP_UDP                1

/* ── DHCP ────────────────────────────────────────────────────────── */
#define LWIP_DHCP               1

/* ── DNS ─────────────────────────────────────────────────────────── */
#define LWIP_DNS                1
#define DNS_TABLE_SIZE          4
#define DNS_MAX_NAME_LENGTH     128

/* ── Socket options ──────────────────────────────────────────────── */
#define LWIP_SO_RCVTIMEO        1
#define LWIP_SO_SNDTIMEO        1

/* ── IPv4 / IPv6 ─────────────────────────────────────────────────── */
#define LWIP_IPV4               1
#define LWIP_IPV6               0       /* IPv4 only for now */

/* ── Loopback ────────────────────────────────────────────────────── */
#define LWIP_HAVE_LOOPIF        1
#define LWIP_NETIF_LOOPBACK     1
#define LWIP_LOOPBACK_MAX_PBUFS 8

/* ── Debugging ───────────────────────────────────────────────────── */
#define LWIP_DEBUG              0
#define LWIP_STATS              0

/* ── Thread sizes ────────────────────────────────────────────────── */
#define TCPIP_THREAD_STACKSIZE  4096
#define TCPIP_THREAD_PRIO       3
#define DEFAULT_THREAD_STACKSIZE 2048
#define DEFAULT_THREAD_PRIO      2
#define TCPIP_MBOX_SIZE         16
#define DEFAULT_ACCEPTMBOX_SIZE 8
#define DEFAULT_RAW_RECVMBOX_SIZE 8
#define DEFAULT_UDP_RECVMBOX_SIZE 8
#define DEFAULT_TCP_RECVMBOX_SIZE 8

/* ── Checksum offload (QEMU: do everything in software) ──────────── */
#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_TCP        1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_TCP      1

#endif /* LWIPOPTS_H */
