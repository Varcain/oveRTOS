/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_SPI_H
#define OVE_HAL_SPI_H

/**
 * @defgroup ove_hal_spi HAL SPI Interface
 * @brief Hardware Abstraction Layer interface for SPI bus operations.
 *
 * Declares the low-level SPI functions that every platform HAL must
 * implement.  The portable @ref ove_spi layer handles bus locking and
 * chip-select management before delegating to these functions.
 * @{
 */

#include "ove/spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure and enable the SPI peripheral.
 *
 * @param[in] spi  SPI handle with storage already assigned.
 * @param[in] cfg  Bus configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg);

/**
 * @brief Disable and release the SPI peripheral.
 *
 * @param[in] spi  SPI handle.
 */
void ove_hal_spi_close(ove_spi_t spi);

/**
 * @brief Full-duplex SPI data transfer.
 *
 * Called under the bus mutex, with CS already asserted by the
 * portable layer.  @p tx or @p rx may be NULL for half-duplex.
 *
 * @param[in]  spi        SPI handle.
 * @param[in]  tx         Transmit buffer, or NULL.
 * @param[out] rx         Receive buffer, or NULL.
 * @param[in]  len        Number of bytes to transfer.
 * @param[in]  timeout_ns Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint64_t timeout_ns);

/**
 * @brief Submit an async SPI transfer.
 *
 * Optional HAL hook. The portable layer calls this only when
 * @c CONFIG_OVE_ASYNC is enabled. CS is asserted by the portable
 * layer beforehand.
 *
 * Backends must call @ref ove_spi_async_complete with the transfer
 * result when the operation finishes. The portable layer then
 * deasserts CS, clears the busy flag, and invokes the user callback.
 *
 * Backends without native DMA support may implement this via a
 * worker thread (POSIX/NuttX), Zephyr signal API, or return
 * @c OVE_ERR_NOT_SUPPORTED in zero-heap modes that cannot allocate
 * the worker resources.
 *
 * @param[in] spi   SPI handle (transfer params can be stashed in the
 *                  pending_tx / pending_rx / pending_len storage
 *                  fields when the HAL needs to defer the work).
 * @param[in] tx    Transmit buffer (may be NULL for read-only).
 * @param[in] rx    Receive buffer (may be NULL for write-only).
 * @param[in] len   Bytes to transfer.
 * @return OVE_OK if accepted, negative error code on submission
 *         failure (no completion in that case).
 */
int ove_hal_spi_transfer_async(ove_spi_t spi, const void *tx, void *rx, size_t len);

/**
 * @brief Completion notification from the HAL.
 *
 * Called by the HAL backend (from worker thread or DMA ISR) when an
 * async transfer finishes. The portable layer takes care of CS
 * deassert, busy-flag clear, and user callback dispatch.
 *
 * Safe to call from ISR context (no mutex unlock, no allocation).
 *
 * @param[in] spi     SPI handle the transfer was submitted on.
 * @param[in] result  OVE_OK on success, negative error code on failure.
 */
void ove_spi_async_complete(ove_spi_t spi, int result);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_SPI_H */
