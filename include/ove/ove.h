/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file ove/ove.h
 * @brief Umbrella header — single include for all oveRTOS public APIs.
 *
 * @defgroup ove_core Core
 * @brief Time, power management, and watchdog — always-on subsystems
 *        for application lifecycle and reliability.
 *
 * @defgroup ove_ui User Interface
 * @brief Console, logging, and interactive shell — operator-facing I/O.
 *
 * @defgroup ove_comm Communication primitives
 * @brief Event groups, stream buffers, and work queues — building blocks
 *        layered above the core thread / sync / queue API.
 *
 * @defgroup ove_data Data storage
 * @brief Filesystem and NVS — persistent storage abstractions.
 *
 * @defgroup ove_umbrella oveRTOS Umbrella Header
 * @brief Convenience header that pulls in the complete oveRTOS public API.
 *
 * Including this single header gives access to every oveRTOS subsystem:
 *
 * | Header                  | Subsystem                                  |
 * |-------------------------|--------------------------------------------|
 * | ove/types.h             | Common types and error codes               |
 * | ove/irq.h               | Critical sections and ISR-context detection|
 * | ove/log.h               | Logging                                    |
 * | ove/thread.h            | Thread management                          |
 * | ove/sync.h              | Mutexes, semaphores, events, condvars      |
 * | ove/audio.h             | Audio graph engine                         |
 * | ove/audio_device.h      | Audio transport / device nodes             |
 * | ove/fs.h                | Filesystem abstraction                     |
 * | ove/queue.h             | Message queues                             |
 * | ove/timer.h             | Software timers                            |
 * | ove/console.h           | Console / serial output                    |
 * | ove/time.h              | Monotonic clock and delays                 |
 * | ove/board.h             | Board initialisation and identification    |
 * | ove/gpio.h              | General-purpose I/O                        |
 * | ove/led.h               | On-board LED control                       |
 * | ove/bsp.h               | Legacy BSP compatibility shim              |
 * | ove/lvgl_internal.h     | LVGL display integration                   |
 * | ove/eventgroup.h        | Event groups (multi-bit flags)             |
 * | ove/workqueue.h         | Deferred work queues                       |
 * | ove/stream.h            | Stream buffers                             |
 * | ove/watchdog.h          | Hardware watchdog timer                    |
 * | ove/nvs.h               | Non-volatile storage                       |
 * | ove/shell.h             | Interactive shell                          |
 * | ove/app.h               | Application lifecycle hooks                |
 * | ove/infer.h             | ML inference (TFLM)                        |
 * | ove/net.h               | TCP/UDP sockets, DNS, network interfaces   |
 * | ove/net_tls.h           | TLS sessions over sockets                  |
 * | ove/net_http.h          | HTTP client                                |
 * | ove/net_mqtt.h          | MQTT client                                |
 * | ove/net_sntp.h          | SNTP time synchronization                  |
 * | ove/net_httpd.h         | HTTP server                                |
 * | ove/uart.h              | UART serial bus driver                     |
 * | ove/spi.h               | SPI bus master driver                      |
 * | ove/i2c.h               | I2C bus master driver                      |
 * | ove/i2s.h               | I2S / SAI audio bus driver                 |
 * | ove/pm.h                | Power management framework                 |
 * | ove/lxp.h               | Linux personality host and providers       |
 *
 * Application code that prefers fine-grained includes may include individual
 * subsystem headers directly instead.
 *
 * @section c_discipline C-binding minimal-overhead discipline
 *
 * The C API IS the substrate every higher-level binding (Rust / Zig /
 * C++) wraps over.  The discipline ported back from the higher
 * bindings — and codified in this header — is:
 *
 *   - **Compile-time C-ABI shape pin.** `<ove/types.h>` carries a
 *     `_Static_assert` block validating every `OVE_ERR_*` numeric
 *     value.  Mirrors `_assert_codes_match` (Rust), the `comptime`
 *     block (Zig), and the `static_assert` block (C++).  A future
 *     re-numbering of an error code fails to compile in every TU
 *     that includes this header.
 *
 *   - **No function-pointer dispatch in hot paths.**  The
 *     `scripts/zero_overhead_audit.py` gate enforces that no
 *     `ove_*_{ops,dispatch,vtable,jumptable,funcs,callbacks}`
 *     data symbol is emitted in any final ELF — every backend selection
 *     happens at link time, not via a runtime jump table.
 *
 *   - **No allocations in hot paths.**  In zero-heap mode
 *     (`CONFIG_OVE_ZERO_HEAP`), `<ove/heap_assert.h>` redeclares the
 *     libc allocators with `__attribute__((error(...)))`, turning
 *     accidental `malloc` / `calloc` / `realloc` reachability into
 *     compile-time failure.
 *
 *   - **Static analysis on demand.**  `make c-analyze` runs GCC's
 *     `-fanalyzer` over the C source tree (Miri analog for C);
 *     filtered to ove paths so third-party noise is suppressed.
 *
 *   - **Sanitizer coverage in CI.**  `make test-stub-sanitize` runs
 *     the C stub tests under UBSan + ASan; mirrors the
 *     `cpp-sanitize` and `rust-miri` CI jobs for binding parity.
 *
 *   - **Cross-language LTO opt-in.**  `OVE_CROSS_LTO=ON` (declared in
 *     `cmake/OveCommon.cmake`) wires `-flto=thin` on the C and C++
 *     sides, `-Clinker-plugin-lto` on Rust, and `-flto` on Zig.  Off
 *     by default — see Gale's "three quiet barriers" note in the
 *     option comment for when to flip it.
 *
 * Use these properties when reasoning about binary size, fault
 * surface, and what guarantees flow through to higher-level bindings.
 *
 * @{
 */

#ifndef OVE_H
#define OVE_H

#include "ove/types.h"
/* heap_assert.h must come AFTER any system header that declares libc
 * allocators (stdlib.h pulled in by types.h).  In zero-heap mode it
 * redeclares malloc / calloc / realloc / zalloc / memalign with
 * __attribute__((error(...))) so subsequent calls fail compilation. */
#include "ove/heap_assert.h"
#include "ove/irq.h"
#include "ove/log.h"
#include "ove/thread.h"
#include "ove/sync.h"
#include "ove/audio.h"
#include "ove/audio_device.h"
#include "ove/fs.h"
#include "ove/queue.h"
#include "ove/timer.h"
#include "ove/console.h"
#include "ove/time.h"
#include "ove/board.h"
#include "ove/block.h"
#include "ove/gpio.h"
#include "ove/led.h"
#include "ove/bsp.h"
#include "ove/lvgl_internal.h"
#include "ove/eventgroup.h"
#include "ove/workqueue.h"
#include "ove/stream.h"
#include "ove/watchdog.h"
#include "ove/nvs.h"
#include "ove/shell.h"
#include "ove/app.h"
#include "ove/infer.h"
#include "ove/net.h"
#include "ove/net_tls.h"
#include "ove/net_http.h"
#include "ove/net_mqtt.h"
#include "ove/net_sntp.h"
#include "ove/net_httpd.h"
#include "ove/uart.h"
#include "ove/spi.h"
#include "ove/i2c.h"
#include "ove/i2s.h"
#include "ove/pm.h"
#include "ove/arena.h"
#include "ove/loader.h"
#include "ove/protected.h"
#if defined(CONFIG_OVE_LINUX)
#include "ove/lxp.h"
#endif

/** @} */

#endif /* OVE_H */
