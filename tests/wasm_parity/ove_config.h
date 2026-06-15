/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * All-modules-on config for the WASM compile-only parity check
 * (driver: `ove lint` -> _wasm_parity).  Fed only to host-gcc
 * -fsyntax-only over the WASM backend sources so signature drift between
 * the WASM stubs and the public headers fails fast.  Never linked.
 */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_RTOS_POSIX 1
#define CONFIG_OVE_BOARD_WASM 1

#define CONFIG_OVE_THREAD 1
#define CONFIG_OVE_SYNC 1
#define CONFIG_OVE_QUEUE 1
#define CONFIG_OVE_TIMER 1
#define CONFIG_OVE_EVENTGROUP 1
#define CONFIG_OVE_STREAM 1
#define CONFIG_OVE_WORKQUEUE 1
#define CONFIG_OVE_WATCHDOG 1
#define CONFIG_OVE_CONSOLE 1
#define CONFIG_OVE_TIME 1

#define CONFIG_OVE_UART 1
#define CONFIG_OVE_SPI 1
#define CONFIG_OVE_I2C 1
#define CONFIG_OVE_I2S 1

#define CONFIG_OVE_NET 1
#define CONFIG_OVE_NET_TLS 1
#define CONFIG_OVE_NET_HTTP 1
#define CONFIG_OVE_NET_MQTT 1
#define CONFIG_OVE_NET_SNTP 1

#define CONFIG_OVE_PROFILER 1
#define CONFIG_OVE_PROFILER_HZ 1000
#define CONFIG_OVE_PROFILER_MAX_DEPTH 32
#define CONFIG_OVE_THREAD_STATE_STATS 1
#define CONFIG_OVE_TRACE_STREAM 1

/* Buffer sizes referenced by the embedded zero-heap storage structs. */
#define CONFIG_OVE_NET_MQTT_RX_BUF 1024
#define CONFIG_OVE_NET_MQTT_TX_BUF 1024
#define CONFIG_OVE_NET_HTTP_MAX_RESPONSE 4096

#endif /* OVE_CONFIG_H */
