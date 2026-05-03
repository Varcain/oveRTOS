/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared lwIP option base for oveRTOS FreeRTOS boards.
 *
 * Boards include this from their <BOARD_DIR>/src/lwipopts.h.  Anything a
 * board needs to override should be #defined before the #include — the
 * defaults below are guarded by #ifndef.  A small set of allocator
 * options are NOT overridable: they pin lwIP into BSS-only mode so a
 * post-`ove_heap_lock()` lwIP call can never spill into wrapped libc
 * malloc.  See plans/overtos-heap-zero-heap-api-split-rustling-axolotl.md
 * for the bounded-pool design.
 *
 * The CONFIG_OVE_NET_LWIP_* knobs may be overridden via the build
 * system (`-DCONFIG_OVE_NET_LWIP_MEM_SIZE=...`) or per-board #define
 * before this include.
 */

#ifndef OVE_LWIPOPTS_COMMON_H
#define OVE_LWIPOPTS_COMMON_H

/* Pull in CONFIG_OVE_NET_LWIP_* values from the generated config. */
#include "ove_config.h"

/* ── Pinned: keep lwIP allocations BSS-only ─────────────────────────
 * MEM_LIBC_MALLOC=1 would route mem_malloc() to libc and bypass MEM_SIZE.
 * MEMP_MEM_MALLOC=1 would route memp pools through mem_malloc().
 * MEM_USE_POOLS=1 would route mem_malloc() to a custom-pool scheme we
 * do not enable.  All three are forced off so the lwIP heap and memp
 * pools live in BSS, sized at compile time, with overflow returning
 * NULL rather than spilling to libc. */
#ifdef MEM_LIBC_MALLOC
#undef MEM_LIBC_MALLOC
#endif
#define MEM_LIBC_MALLOC 0

#ifdef MEMP_MEM_MALLOC
#undef MEMP_MEM_MALLOC
#endif
#define MEMP_MEM_MALLOC 0

#ifdef MEM_USE_POOLS
#undef MEM_USE_POOLS
#endif
#define MEM_USE_POOLS 0

#ifdef LWIP_RAND_FROM_LIBC
#undef LWIP_RAND_FROM_LIBC
#endif
#define LWIP_RAND_FROM_LIBC 0

/* ── Platform / OS ──────────────────────────────────────────────── */
#ifndef NO_SYS
#define NO_SYS 0
#endif
#ifndef LWIP_SOCKET
#define LWIP_SOCKET 1
#endif
#ifndef LWIP_NETCONN
#define LWIP_NETCONN 1
#endif
#ifndef LWIP_COMPAT_SOCKETS
#define LWIP_COMPAT_SOCKETS 0
#endif

/* ── Memory sizing (pool ceilings — Kconfig-driven) ─────────────── */
#ifndef MEM_ALIGNMENT
#define MEM_ALIGNMENT 4
#endif

#ifndef CONFIG_OVE_NET_LWIP_MEM_SIZE
#define CONFIG_OVE_NET_LWIP_MEM_SIZE (16 * 1024)
#endif
#ifndef MEM_SIZE
#define MEM_SIZE CONFIG_OVE_NET_LWIP_MEM_SIZE
#endif

#ifndef CONFIG_OVE_NET_LWIP_PBUF_POOL_SIZE
#define CONFIG_OVE_NET_LWIP_PBUF_POOL_SIZE 16
#endif
#ifndef PBUF_POOL_SIZE
#define PBUF_POOL_SIZE CONFIG_OVE_NET_LWIP_PBUF_POOL_SIZE
#endif

#ifndef CONFIG_OVE_NET_LWIP_PBUF_BUFSIZE
#define CONFIG_OVE_NET_LWIP_PBUF_BUFSIZE 1536
#endif
#ifndef PBUF_POOL_BUFSIZE
#define PBUF_POOL_BUFSIZE CONFIG_OVE_NET_LWIP_PBUF_BUFSIZE
#endif

#ifndef CONFIG_OVE_NET_LWIP_NUM_TCP_PCB
#define CONFIG_OVE_NET_LWIP_NUM_TCP_PCB 16
#endif
#ifndef MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB CONFIG_OVE_NET_LWIP_NUM_TCP_PCB
#endif

#ifndef CONFIG_OVE_NET_LWIP_NUM_TCP_PCB_LISTEN
#define CONFIG_OVE_NET_LWIP_NUM_TCP_PCB_LISTEN 4
#endif
#ifndef MEMP_NUM_TCP_PCB_LISTEN
#define MEMP_NUM_TCP_PCB_LISTEN CONFIG_OVE_NET_LWIP_NUM_TCP_PCB_LISTEN
#endif

#ifndef CONFIG_OVE_NET_LWIP_NUM_UDP_PCB
#define CONFIG_OVE_NET_LWIP_NUM_UDP_PCB 8
#endif
#ifndef MEMP_NUM_UDP_PCB
#define MEMP_NUM_UDP_PCB CONFIG_OVE_NET_LWIP_NUM_UDP_PCB
#endif

#ifndef CONFIG_OVE_NET_LWIP_NUM_NETCONN
#define CONFIG_OVE_NET_LWIP_NUM_NETCONN 16
#endif
#ifndef MEMP_NUM_NETCONN
#define MEMP_NUM_NETCONN CONFIG_OVE_NET_LWIP_NUM_NETCONN
#endif

#ifndef MEMP_NUM_PBUF
#define MEMP_NUM_PBUF 16
#endif

/* ── TCP defaults ───────────────────────────────────────────────── */
#ifndef LWIP_TCP
#define LWIP_TCP 1
#endif
#ifndef TCP_MSS
#define TCP_MSS 1460
#endif
#ifndef TCP_SND_BUF
#define TCP_SND_BUF (4 * TCP_MSS)
#endif
#ifndef TCP_WND
#define TCP_WND (4 * TCP_MSS)
#endif

/* ── UDP / DHCP / DNS ───────────────────────────────────────────── */
#ifndef LWIP_UDP
#define LWIP_UDP 1
#endif
#ifndef LWIP_DHCP
#define LWIP_DHCP 1
#endif
#ifndef LWIP_DNS
#define LWIP_DNS 1
#endif

#ifndef CONFIG_OVE_NET_LWIP_DNS_TABLE_SIZE
#define CONFIG_OVE_NET_LWIP_DNS_TABLE_SIZE 4
#endif
#ifndef DNS_TABLE_SIZE
#define DNS_TABLE_SIZE CONFIG_OVE_NET_LWIP_DNS_TABLE_SIZE
#endif

#ifndef DNS_MAX_NAME_LENGTH
#define DNS_MAX_NAME_LENGTH 128
#endif

/* ── Socket options ─────────────────────────────────────────────── */
#ifndef LWIP_SO_RCVTIMEO
#define LWIP_SO_RCVTIMEO 1
#endif
#ifndef LWIP_SO_SNDTIMEO
#define LWIP_SO_SNDTIMEO 1
#endif

/* ── IPv4 / IPv6 ────────────────────────────────────────────────── */
#ifndef LWIP_IPV4
#define LWIP_IPV4 1
#endif
#ifndef LWIP_IPV6
#define LWIP_IPV6 0
#endif

/* ── Loopback ───────────────────────────────────────────────────── */
#ifndef LWIP_HAVE_LOOPIF
#define LWIP_HAVE_LOOPIF 1
#endif
#ifndef LWIP_NETIF_LOOPBACK
#define LWIP_NETIF_LOOPBACK 1
#endif
#ifndef LWIP_LOOPBACK_MAX_PBUFS
#define LWIP_LOOPBACK_MAX_PBUFS 8
#endif

/* ── Debug / stats ──────────────────────────────────────────────── */
#ifndef LWIP_DEBUG
#define LWIP_DEBUG 0
#endif
#ifndef LWIP_STATS
#define LWIP_STATS 0
#endif

/* ── Thread sizes ───────────────────────────────────────────────── */
#ifndef TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE 4096
#endif
#ifndef TCPIP_THREAD_PRIO
#define TCPIP_THREAD_PRIO 3
#endif
#ifndef DEFAULT_THREAD_STACKSIZE
#define DEFAULT_THREAD_STACKSIZE 2048
#endif
#ifndef DEFAULT_THREAD_PRIO
#define DEFAULT_THREAD_PRIO 2
#endif
#ifndef TCPIP_MBOX_SIZE
#define TCPIP_MBOX_SIZE 16
#endif
#ifndef DEFAULT_ACCEPTMBOX_SIZE
#define DEFAULT_ACCEPTMBOX_SIZE 8
#endif
#ifndef DEFAULT_RAW_RECVMBOX_SIZE
#define DEFAULT_RAW_RECVMBOX_SIZE 8
#endif
#ifndef DEFAULT_UDP_RECVMBOX_SIZE
#define DEFAULT_UDP_RECVMBOX_SIZE 8
#endif
#ifndef DEFAULT_TCP_RECVMBOX_SIZE
#define DEFAULT_TCP_RECVMBOX_SIZE 8
#endif

/* ── Software checksums (boards with HW offload override these) ─ */
#ifndef CHECKSUM_GEN_IP
#define CHECKSUM_GEN_IP 1
#endif
#ifndef CHECKSUM_GEN_UDP
#define CHECKSUM_GEN_UDP 1
#endif
#ifndef CHECKSUM_GEN_TCP
#define CHECKSUM_GEN_TCP 1
#endif
#ifndef CHECKSUM_CHECK_IP
#define CHECKSUM_CHECK_IP 1
#endif
#ifndef CHECKSUM_CHECK_UDP
#define CHECKSUM_CHECK_UDP 1
#endif
#ifndef CHECKSUM_CHECK_TCP
#define CHECKSUM_CHECK_TCP 1
#endif

#endif /* OVE_LWIPOPTS_COMMON_H */
