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
 * @defgroup ove_umbrella oveRTOS Umbrella Header
 * @brief Convenience header that pulls in the complete oveRTOS public API.
 *
 * Including this single header gives access to every oveRTOS subsystem:
 *
 * | Header                  | Subsystem                                  |
 * |-------------------------|--------------------------------------------|
 * | ove/types.h             | Common types and error codes               |
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
 *
 * Application code that prefers fine-grained includes may include individual
 * subsystem headers directly instead.
 * @{
 */

#ifndef OVE_H
#define OVE_H

#include "ove/types.h"
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

/** @} */

#endif /* OVE_H */
