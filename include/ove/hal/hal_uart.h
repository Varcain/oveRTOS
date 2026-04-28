/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_UART_H
#define OVE_HAL_UART_H

/**
 * @defgroup ove_hal_uart HAL UART Interface
 * @brief Hardware Abstraction Layer interface for UART operations.
 *
 * Declares the low-level UART functions that every platform HAL must
 * implement.  The portable @ref ove_uart layer handles RX buffering
 * (via @ref ove_stream) and TX locking.
 *
 * The HAL does @b not manage the RX buffer.  Instead, the backend's
 * RX ISR calls @ref ove_uart_rx_isr_push() to deliver bytes to the
 * portable layer's internal stream.
 * @{
 */

#include "ove/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure and enable the UART peripheral.
 *
 * Maps the instance index to hardware, configures baud rate and
 * framing, and enables the RX interrupt path.
 *
 * @param[in] uart  UART handle with storage already assigned.
 * @param[in] cfg   UART configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_uart_open(ove_uart_t uart, const struct ove_uart_cfg *cfg);

/**
 * @brief Disable and release the UART peripheral.
 *
 * @param[in] uart  UART handle.
 */
void ove_hal_uart_close(ove_uart_t uart);

/**
 * @brief Blocking transmit of data.
 *
 * Called under the TX mutex.
 *
 * @param[in]  uart          UART handle.
 * @param[in]  data          Data to transmit.
 * @param[in]  len           Number of bytes.
 * @param[in]  timeout_ms    Maximum wait time.
 * @param[out] bytes_written Actual bytes written, or NULL.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_uart_tx(ove_uart_t uart, const void *data, size_t len, uint32_t timeout_ms,
		    size_t *bytes_written);

/**
 * @brief Enable the RX interrupt / start RX reception.
 *
 * Called after the RX stream is set up.  The ISR should call
 * @ref ove_uart_rx_isr_push() for each received byte.
 *
 * @param[in] uart  UART handle.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_uart_rx_enable(ove_uart_t uart);

/**
 * @brief Flush the TX hardware FIFO.
 *
 * Blocks until all pending bytes have been physically transmitted.
 *
 * @param[in] uart  UART handle.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_uart_tx_flush(ove_uart_t uart);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_UART_H */
