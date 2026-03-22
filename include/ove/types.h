/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_types Types
 * @brief Common error codes, opaque handle types, and utility macros used
 *        throughout the oveRTOS public API.
 * @{
 */

#ifndef OVE_TYPES_H
#define OVE_TYPES_H

#include <stddef.h>
#include <stdint.h>

/** @brief Operation completed successfully. */
#define OVE_OK                    0

/** @brief The requested backend or feature has not been registered. */
#define OVE_ERR_NOT_REGISTERED   (-1)

/** @brief One or more parameters are invalid (e.g. NULL pointer, zero size). */
#define OVE_ERR_INVALID_PARAM    (-2)

/** @brief Dynamic allocation failed — heap is full or not available. */
#define OVE_ERR_NO_MEMORY        (-3)

/** @brief Operation timed out before it could complete. */
#define OVE_ERR_TIMEOUT          (-4)

/** @brief The requested feature is not supported by the active backend. */
#define OVE_ERR_NOT_SUPPORTED    (-5)

/** @brief Queue send failed because the queue is at maximum capacity. */
#define OVE_ERR_QUEUE_FULL       (-6)

/**
 * @brief Timeout value that means "block indefinitely".
 *
 * Pass this as the @c timeout_ms argument to any blocking API to wait
 * without a deadline.
 */
#define OVE_WAIT_FOREVER          UINT32_MAX

/** @brief Opaque handle for a thread object. @see ove_thread_init, ove_thread_create */
typedef struct ove_thread *ove_thread_t;

/** @brief Opaque handle for a mutex object. @see ove_mutex_init, ove_mutex_create */
typedef struct ove_mutex *ove_mutex_t;

/** @brief Opaque handle for a counting semaphore object. @see ove_sem_init, ove_sem_create */
typedef struct ove_sem *ove_sem_t;

/** @brief Opaque handle for a binary event (signal/wait) object. @see ove_event_init, ove_event_create */
typedef struct ove_event *ove_event_t;

/** @brief Opaque handle for a condition variable object. @see ove_condvar_init, ove_condvar_create */
typedef struct ove_condvar *ove_condvar_t;

/** @brief Opaque handle for an event-group (bit-field) object. */
typedef struct ove_eventgroup *ove_eventgroup_t;

/** @brief Opaque handle for a work queue object. */
typedef struct ove_workqueue *ove_workqueue_t;

/** @brief Opaque handle for a deferred work item. */
typedef struct ove_work *ove_work_t;

/** @brief Opaque handle for a byte-stream (ring-buffer) object. */
typedef struct ove_stream *ove_stream_t;

/** @brief Opaque handle for a software watchdog object. */
typedef struct ove_watchdog *ove_watchdog_t;

/** @brief Opaque handle for an open file. */
typedef struct ove_file *ove_file_t;

/** @brief Opaque handle for an open directory. */
typedef struct ove_dir *ove_dir_t;

/**
 * @brief Bit-mask type used by the event-group API.
 *
 * Each bit represents a distinct event flag.  Up to 32 independent flags
 * can be combined in a single event group.
 */
typedef uint32_t ove_eventbits_t;

/* RTOS name string (compile-time) */
#include "ove_config.h"

/**
 * @brief Compile-time string identifying the active RTOS backend.
 *
 * Resolved to a human-readable string literal ("FreeRTOS", "Zephyr",
 * "NuttX", "POSIX", or "Unknown") based on which
 * @c CONFIG_OVE_RTOS_* symbol is defined in @c ove_config.h.
 */
#if defined(CONFIG_OVE_RTOS_FREERTOS)
#define OVE_RTOS_NAME "FreeRTOS"
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#define OVE_RTOS_NAME "Zephyr"
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#define OVE_RTOS_NAME "NuttX"
#elif defined(CONFIG_OVE_RTOS_POSIX)
#define OVE_RTOS_NAME "POSIX"
#else
#define OVE_RTOS_NAME "Unknown"
#endif

#endif /* OVE_TYPES_H */

/** @} */
