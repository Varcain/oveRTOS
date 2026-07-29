/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file types.h
 * @defgroup ove_types Types
 * @brief Common error codes, opaque handle types, and utility macros used
 *        throughout the oveRTOS public API.
 * @{
 */

#ifndef OVE_TYPES_H
#define OVE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "oveRTOS requires a GCC-compatible compiler (gcc or clang). Other toolchains (MSVC, etc.) are not supported: the substrate uses __attribute__ extensions, statement expressions, and other GCC-flavoured features throughout."
#endif

/*
 * Public-API annotation macros.
 *
 * The skip-list (bindgen / Zig @cImport / Emscripten / clang-tidy lint)
 * mirrors heap_assert.h: those tools either don't grok the attribute
 * cleanly or never produce a binary the annotation could affect, so
 * expand to nothing there. Mirroring the existing pattern keeps the
 * tooling surface consistent.
 */
#if defined(__BINDGEN__) || defined(__ZIG_CIMPORT__) || defined(__EMSCRIPTEN__) || \
	defined(__OVE_LINT__)
#define OVE_NONNULL(...)
#define OVE_NODISCARD
#define OVE_RETURNS_NONNULL
#define OVE_DEPRECATED(msg)
#else
/**
 * @brief Mark required-non-NULL pointer parameters (1-indexed positions).
 *
 * Callers passing @c NULL trigger a compile-time @c -Wnonnull warning.
 * Caution: GCC may use this as an axiom and eliminate subsequent
 * @c NULL checks inside the callee body — annotation and defensive
 * @c if (!ptr) checks must be kept consistent or one will silently
 * disappear.
 */
#define OVE_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))

/**
 * @brief Warn at compile time if the return value is discarded.
 *
 * Modern GCC does NOT honour the @c (void) cast for this attribute.
 * Callers that genuinely don't care must consume the return via an
 * assigned variable: @c int rc = func(); (void)rc;
 */
#define OVE_NODISCARD __attribute__((warn_unused_result))

/** @brief Mark a function as never returning a NULL pointer. */
#define OVE_RETURNS_NONNULL __attribute__((returns_nonnull))

/** @brief Mark an API deprecated with an explanatory message. */
#define OVE_DEPRECATED(msg) __attribute__((deprecated(msg)))
#endif

/**
 * @brief oveRTOS error codes.
 *
 * Convention: zero (@c OVE_OK) on success, negative values on error.
 * Numeric values are pinned by the @c _Static_assert block below — the
 * names and codes form the stable C ABI between substrate and bindings.
 *
 * Function APIs return @c int (not @c ove_err_t) for ABI compatibility
 * and to keep the @c int rc = ...; if (rc < 0) idiom unchanged.
 * Bindings consume the typed enum via bindgen / @c \@cImport.
 */
typedef enum ove_err {
	/** Operation completed successfully. */
	OVE_OK = 0,

	/** The requested backend or feature has not been registered. */
	OVE_ERR_NOT_REGISTERED = -1,

	/** One or more parameters are invalid (e.g. NULL pointer, zero size). */
	OVE_ERR_INVALID_PARAM = -2,

	/** Dynamic allocation failed — heap is full or not available. */
	OVE_ERR_NO_MEMORY = -3,

	/** Operation timed out before it could complete. */
	OVE_ERR_TIMEOUT = -4,

	/** The requested feature is not supported by the active backend. */
	OVE_ERR_NOT_SUPPORTED = -5,

	/** Queue send failed because the queue is at maximum capacity. */
	OVE_ERR_QUEUE_FULL = -6,

	/** ML inference or model loading failed. */
	OVE_ERR_ML_FAILED = -7,

	/** Remote peer refused the connection. */
	OVE_ERR_NET_REFUSED = -8,

	/** Network or host is unreachable. */
	OVE_ERR_NET_UNREACHABLE = -9,

	/** Local address already in use. */
	OVE_ERR_NET_ADDR_IN_USE = -10,

	/** Connection was reset by the remote peer. */
	OVE_ERR_NET_RESET = -11,

	/** DNS name resolution failed. */
	OVE_ERR_NET_DNS_FAIL = -12,

	/** Connection closed by the remote peer. */
	OVE_ERR_NET_CLOSED = -13,

	/** Bus device did not acknowledge (I2C NACK). */
	OVE_ERR_BUS_NACK = -14,

	/** Bus arbitration lost (multi-master). */
	OVE_ERR_BUS_BUSY = -15,

	/** Framing, parity, or hardware error on a serial bus. */
	OVE_ERR_BUS_ERROR = -16,

	/** Queue receive failed because the queue is empty. */
	OVE_ERR_QUEUE_EMPTY = -17,

	/**
	 * Non-blocking operation would have to block.
	 *
	 * Returned by zero-timeout / ISR variants of blocking APIs when the
	 * requested resource is unavailable.  More general than @c OVE_ERR_QUEUE_FULL
	 * / @c OVE_ERR_QUEUE_EMPTY; suitable for mutex/sem/etc. non-blocking paths.
	 */
	OVE_ERR_WOULD_BLOCK = -18,

	/** End of file / directory iterator exhausted. */
	OVE_ERR_EOF = -19,

	/** Argument or state is invalid for this operation. */
	OVE_ERR_INVAL = -20,

	/** Requested key / entry / resource was not found. */
	OVE_ERR_NOT_FOUND = -21,

	/** Requested local network address is not configured on this host. */
	OVE_ERR_NET_ADDR_NOT_AVAILABLE = -22,
} ove_err_t;

/**
 * @brief Timeout value that means "block indefinitely".
 *
 * Pass this as the @c timeout_ns argument to any blocking API (or as the
 * @c deadline_ns argument to an @c _until variant) to wait without a
 * deadline.
 */
#define OVE_WAIT_FOREVER UINT64_MAX

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
OVE_STATIC_ASSERT(OVE_ERR_QUEUE_EMPTY == -17, "OVE_ERR_QUEUE_EMPTY drifted");
OVE_STATIC_ASSERT(OVE_ERR_WOULD_BLOCK == -18, "OVE_ERR_WOULD_BLOCK drifted");
OVE_STATIC_ASSERT(OVE_ERR_EOF == -19, "OVE_ERR_EOF drifted");
OVE_STATIC_ASSERT(OVE_ERR_INVAL == -20, "OVE_ERR_INVAL drifted");
OVE_STATIC_ASSERT(OVE_ERR_NOT_FOUND == -21, "OVE_ERR_NOT_FOUND drifted");
OVE_STATIC_ASSERT(OVE_ERR_NET_ADDR_NOT_AVAILABLE == -22,
		  "OVE_ERR_NET_ADDR_NOT_AVAILABLE drifted");
OVE_STATIC_ASSERT(OVE_WAIT_FOREVER == UINT64_MAX, "OVE_WAIT_FOREVER drifted");

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

/**
 * @brief Notify-callback signature used by the @c _set_notify variants of
 *        the comm primitives (stream / queue / eventgroup / semaphore).
 *
 * Invoked from inside the producing call (e.g. @c ove_stream_send) after a
 * successful update.  The implementation must be short, non-blocking, and
 * safe to call from whatever context the originating send/give/set ran
 * in — typically a Rust @c AtomicWaker::wake bridge for the async runtime.
 *
 * @note The @c _set_notify registration is *setup-time only*: it is NOT safe
 *       to call concurrently with producing operations on the same primitive,
 *       since those read the callback pointer without synchronisation. Register
 *       the callback once before the primitive is shared across contexts (the
 *       async bridge registers it at construction).
 *
 * @param[in] user_data Opaque pointer supplied at @c _set_notify time.
 */
typedef void (*ove_notify_cb)(void *user_data);

/**
 * @brief DMA / async transfer completion callback signature.
 *
 * Invoked by @c ove_spi_transfer_async / @c ove_i2c_write_read_async
 * (and similar) when a transfer finishes.  Runs in whatever context
 * the backend's completion fires in: ISR on real DMA-capable
 * hardware (STM32F7 HAL), thread on simulator / worker-thread
 * fallback paths. Implementations must be non-blocking and ISR-safe.
 *
 * @param[in] result     OVE_OK on success, negative @c OVE_ERR_* on failure.
 * @param[in] user_data  Opaque pointer passed to the @c _async call.
 */
typedef void (*ove_dma_complete_cb)(int result, void *user_data);

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
