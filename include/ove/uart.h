/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_UART_H
#define OVE_UART_H

/**
 * @defgroup ove_uart UART
 * @brief UART serial bus driver.
 *
 * Provides a portable, multi-instance UART API with configurable baud
 * rate and framing, interrupt-driven RX buffering, thread-safe TX, and
 * blocking read/write with timeout.
 *
 * RX bytes are buffered internally via an @ref ove_stream.  Backend ISR
 * handlers push received bytes through @ref ove_uart_rx_isr_push() which
 * the portable layer provides.
 *
 * Two allocation strategies are supported:
 * - @c _create() / @c _destroy() — heap-allocated.  Available only when
 *   @c OVE_HEAP_UART is defined (i.e. @c CONFIG_OVE_ZERO_HEAP is not set).
 * - @c _init() / @c _deinit() — caller-supplied storage and RX buffer.
 *   Available in both modes.  See @c OVE_UART_DEFINE_STATIC for a one-step
 *   static helper.
 *
 * @note Requires @c CONFIG_OVE_UART.
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"
#include "ove/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Enums ───────────────────────────────────────────────────────── */

/** @brief UART parity mode. */
typedef enum {
	OVE_UART_PARITY_NONE = 0,
	OVE_UART_PARITY_ODD = 1,
	OVE_UART_PARITY_EVEN = 2,
} ove_uart_parity_t;

/** @brief UART stop-bit count. */
typedef enum {
	OVE_UART_STOP_1 = 0,
	OVE_UART_STOP_1_5 = 1,
	OVE_UART_STOP_2 = 2,
} ove_uart_stop_t;

/** @brief UART hardware flow control. */
typedef enum {
	OVE_UART_FLOW_NONE = 0,
	OVE_UART_FLOW_RTS_CTS = 1,
} ove_uart_flow_t;

/* ── Configuration ───────────────────────────────────────────────── */

/**
 * @brief UART configuration descriptor.
 */
struct ove_uart_cfg {
	unsigned int instance;	      /**< Peripheral index (0, 1, 2 ...). */
	uint32_t baudrate;	      /**< Baud rate in bps (e.g. 115200). */
	uint8_t data_bits;	      /**< Data bits: 7, 8, or 9. */
	ove_uart_parity_t parity;     /**< Parity mode. */
	ove_uart_stop_t stop_bits;    /**< Stop bit count. */
	ove_uart_flow_t flow_control; /**< Hardware flow control. */
	size_t rx_buf_size;	      /**< RX ring buffer size in bytes. */
};

#ifdef CONFIG_OVE_UART

/* ── Lifecycle ───────────────────────────────────────────────────── */

/**
 * @brief Initialise a UART using caller-provided static storage.
 *
 * @param[out] uart    Receives the initialised UART handle.
 * @param[in]  storage Pointer to statically-allocated UART storage.
 * @param[in]  rx_buf  Caller-supplied RX ring buffer.
 * @param[in]  cfg     UART configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_uart_init(ove_uart_t *uart, ove_uart_storage_t *storage, void *rx_buf,
		  const struct ove_uart_cfg *cfg);

/** @brief Release a UART handle previously created with `ove_uart_init`. */
void ove_uart_deinit(ove_uart_t uart);

#ifdef OVE_HEAP_UART
/**
 * @brief Heap-mode counterpart of `ove_uart_init()` — allocates storage and RX buffer.
 * @param[out] uart Receives the initialised handle.
 * @param[in]  cfg  UART configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_uart_create(ove_uart_t *uart, const struct ove_uart_cfg *cfg);
/** @brief Destroy a UART handle previously created with `ove_uart_create`. */
void ove_uart_destroy(ove_uart_t uart);
#endif /* OVE_HEAP_UART */

/* ── Operations ──────────────────────────────────────────────────── */

/**
 * @brief Write data to the UART.
 *
 * Blocks for up to @p timeout_ms until all bytes are accepted.
 * Thread-safe (internal TX mutex).
 *
 * @param[in]  uart          UART handle.
 * @param[in]  data          Data to transmit.
 * @param[in]  len           Number of bytes to write.
 * @param[in]  timeout_ms    Maximum wait time.
 * @param[out] bytes_written Actual bytes written, or NULL.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_uart_write(ove_uart_t uart, const void *data, size_t len, uint32_t timeout_ms,
		   size_t *bytes_written);

/**
 * @brief Read data from the UART RX buffer.
 *
 * Blocks for up to @p timeout_ms until at least 1 byte is available.
 *
 * @param[in]  uart         UART handle.
 * @param[out] buf          Buffer to receive data.
 * @param[in]  len          Maximum bytes to read.
 * @param[in]  timeout_ms   Maximum wait time.
 * @param[out] bytes_read   Actual bytes read, or NULL.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_uart_read(ove_uart_t uart, void *buf, size_t len, uint32_t timeout_ms, size_t *bytes_read);

/**
 * @brief Query the number of bytes available in the RX buffer.
 *
 * @param[in] uart  UART handle.
 * @return Number of bytes available, or 0 if empty or invalid.
 */
size_t ove_uart_bytes_available(ove_uart_t uart);

/**
 * @brief Flush the TX hardware buffer.
 *
 * Blocks until all pending TX bytes have been physically transmitted.
 *
 * @param[in] uart  UART handle.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_uart_flush(ove_uart_t uart);

/* ── ISR helper (called by backend, not by application code) ─────── */

/**
 * @brief Push received bytes from ISR into the portable RX buffer.
 *
 * Backend UART ISR handlers call this function to deliver received
 * bytes to the portable layer's internal @ref ove_stream.
 *
 * @param[in] uart  UART handle.
 * @param[in] data  Pointer to received byte(s).
 * @param[in] len   Number of bytes received.
 */
void ove_uart_rx_isr_push(ove_uart_t uart, const void *data, size_t len);

#else /* !CONFIG_OVE_UART */

/* No _init/_deinit stubs: OVE_UART_DEFINE_STATIC is itself gated by
 * #ifdef CONFIG_OVE_UART in storage.h. */
static inline int ove_uart_create(ove_uart_t *u, const struct ove_uart_cfg *c)
{
	(void)u;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_uart_destroy(ove_uart_t u)
{
	(void)u;
}
static inline int ove_uart_write(ove_uart_t u, const void *d, size_t l, uint32_t t, size_t *bw)
{
	(void)u;
	(void)d;
	(void)l;
	(void)t;
	(void)bw;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_uart_read(ove_uart_t u, void *b, size_t l, uint32_t t, size_t *br)
{
	(void)u;
	(void)b;
	(void)l;
	(void)t;
	(void)br;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline size_t ove_uart_bytes_available(ove_uart_t u)
{
	(void)u;
	return 0;
}
static inline int ove_uart_flush(ove_uart_t u)
{
	(void)u;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_uart_rx_isr_push(ove_uart_t u, const void *d, size_t l)
{
	(void)u;
	(void)d;
	(void)l;
}

#endif /* CONFIG_OVE_UART */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_UART_H */
