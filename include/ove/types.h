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
#define OVE_OK 0

/** @brief The requested backend or feature has not been registered. */
#define OVE_ERR_NOT_REGISTERED (-1)

/** @brief One or more parameters are invalid (e.g. NULL pointer, zero size). */
#define OVE_ERR_INVALID_PARAM (-2)

/** @brief Dynamic allocation failed — heap is full or not available. */
#define OVE_ERR_NO_MEMORY (-3)

/** @brief Operation timed out before it could complete. */
#define OVE_ERR_TIMEOUT (-4)

/** @brief The requested feature is not supported by the active backend. */
#define OVE_ERR_NOT_SUPPORTED (-5)

/** @brief Queue send failed because the queue is at maximum capacity. */
#define OVE_ERR_QUEUE_FULL (-6)

/** @brief ML inference or model loading failed. */
#define OVE_ERR_ML_FAILED (-7)

/** @brief Remote peer refused the connection. */
#define OVE_ERR_NET_REFUSED (-8)

/** @brief Network or host is unreachable. */
#define OVE_ERR_NET_UNREACHABLE (-9)

/** @brief Local address already in use. */
#define OVE_ERR_NET_ADDR_IN_USE (-10)

/** @brief Connection was reset by the remote peer. */
#define OVE_ERR_NET_RESET (-11)

/** @brief DNS name resolution failed. */
#define OVE_ERR_NET_DNS_FAIL (-12)

/** @brief Connection closed by the remote peer. */
#define OVE_ERR_NET_CLOSED (-13)

/** @brief Bus device did not acknowledge (I2C NACK). */
#define OVE_ERR_BUS_NACK (-14)

/** @brief Bus arbitration lost (multi-master). */
#define OVE_ERR_BUS_BUSY (-15)

/** @brief Framing, parity, or hardware error on a serial bus. */
#define OVE_ERR_BUS_ERROR (-16)

/**
 * @brief Timeout value that means "block indefinitely".
 *
 * Pass this as the @c timeout_ms argument to any blocking API to wait
 * without a deadline.
 */
#define OVE_WAIT_FOREVER UINT32_MAX

/*
 * Compile-time C-ABI shape check.  This file IS the C-ABI contract;
 * the assertions below pin the numeric values that every binding
 * (Rust `Error::from_code`, Zig `mapErrorCode`, C++ `static_assert`
 * block in <ove/types.hpp>) reads back.  If a future re-numbering
 * drifts an `OVE_ERR_*` value, every TU that includes this header
 * fails to compile with a clear message — the silent-drift surface is
 * closed at the substrate layer the higher-level bindings sit on.
 *
 * `_Static_assert` is the C99/C11 keyword (also a GCC extension in
 * C++); `static_assert` is the C++11 keyword.  Pick whichever the
 * including TU's language supports — TUs that include this header as
 * C++ (the C++ binding's <ove/types.hpp> for instance) need the
 * portable spelling.
 */
#if defined(__cplusplus)
#define OVE_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define OVE_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
OVE_STATIC_ASSERT(OVE_ERR_NOT_REGISTERED == -1, "OVE_ERR_NOT_REGISTERED drifted");
OVE_STATIC_ASSERT(OVE_ERR_INVALID_PARAM == -2, "OVE_ERR_INVALID_PARAM drifted");
OVE_STATIC_ASSERT(OVE_ERR_NO_MEMORY == -3, "OVE_ERR_NO_MEMORY drifted");
OVE_STATIC_ASSERT(OVE_ERR_TIMEOUT == -4, "OVE_ERR_TIMEOUT drifted");
OVE_STATIC_ASSERT(OVE_ERR_NOT_SUPPORTED == -5, "OVE_ERR_NOT_SUPPORTED drifted");
OVE_STATIC_ASSERT(OVE_ERR_QUEUE_FULL == -6, "OVE_ERR_QUEUE_FULL drifted");
OVE_STATIC_ASSERT(OVE_ERR_ML_FAILED == -7, "OVE_ERR_ML_FAILED drifted");
OVE_STATIC_ASSERT(OVE_ERR_NET_REFUSED == -8, "OVE_ERR_NET_REFUSED drifted");
OVE_STATIC_ASSERT(OVE_ERR_NET_UNREACHABLE == -9, "OVE_ERR_NET_UNREACHABLE drifted");
OVE_STATIC_ASSERT(OVE_ERR_NET_ADDR_IN_USE == -10, "OVE_ERR_NET_ADDR_IN_USE drifted");
OVE_STATIC_ASSERT(OVE_ERR_NET_RESET == -11, "OVE_ERR_NET_RESET drifted");
OVE_STATIC_ASSERT(OVE_ERR_NET_DNS_FAIL == -12, "OVE_ERR_NET_DNS_FAIL drifted");
OVE_STATIC_ASSERT(OVE_ERR_NET_CLOSED == -13, "OVE_ERR_NET_CLOSED drifted");
OVE_STATIC_ASSERT(OVE_ERR_BUS_NACK == -14, "OVE_ERR_BUS_NACK drifted");
OVE_STATIC_ASSERT(OVE_ERR_BUS_BUSY == -15, "OVE_ERR_BUS_BUSY drifted");
OVE_STATIC_ASSERT(OVE_ERR_BUS_ERROR == -16, "OVE_ERR_BUS_ERROR drifted");

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

/** @brief Opaque handle for an ML inference model session. @see ove_model_init, ove_model_create */
typedef struct ove_model *ove_model_t;

/** @brief Opaque handle for a network socket. @see ove_socket_open, ove_socket_create */
typedef struct ove_socket *ove_socket_t;

/** @brief Opaque handle for a network interface. @see ove_netif_init, ove_netif_create */
typedef struct ove_netif *ove_netif_t;

/** @brief Opaque handle for a TLS session. @see ove_tls_init, ove_tls_create */
typedef struct ove_tls *ove_tls_t;

/** @brief Opaque handle for an HTTP client. @see ove_http_client_init, ove_http_client_create */
typedef struct ove_http_client *ove_http_client_t;

/** @brief Opaque handle for an MQTT client. @see ove_mqtt_client_init, ove_mqtt_client_create */
typedef struct ove_mqtt_client *ove_mqtt_client_t;

/** @brief Opaque handle for a UART peripheral. @see ove_uart_init, ove_uart_create */
typedef struct ove_uart *ove_uart_t;

/** @brief Opaque handle for an SPI bus controller. @see ove_spi_init, ove_spi_create */
typedef struct ove_spi *ove_spi_t;

/** @brief Opaque handle for an I2C bus controller. @see ove_i2c_init, ove_i2c_create */
typedef struct ove_i2c *ove_i2c_t;

/** @brief Opaque handle for an I2S / SAI bus controller. @see ove_i2s_init, ove_i2s_create */
typedef struct ove_i2s *ove_i2s_t;

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
