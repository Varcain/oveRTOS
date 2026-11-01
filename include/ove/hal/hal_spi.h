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
 * @brief Validate backend transfer limits without changing any state.
 *
 * The portable layer has checked pointer presence, nonzero whole-frame byte
 * count, alignment and overlap. It calls this hook before taking its mutex,
 * asserting software CS or publishing asynchronous transfer state, including
 * every nonempty record of a sequence. Do not perform I/O, allocate, or retain
 * arguments here. The configured SPI handle must remain stable across the
 * validation and transfer. Implementations may reject a one-shot async count
 * that their synchronous path can split into multiple native calls.
 *
 * @param[in] spi           Configured SPI handle.
 * @param[in] len           Number of bytes, not native words.
 * @param[in] asynchronous  Nonzero for an asynchronous submission.
 * @return OVE_OK if admissible, otherwise a negative error code.
 */
int ove_hal_spi_validate_transfer(ove_spi_t spi, size_t len, int asynchronous);

/**
 * @brief Full-duplex SPI data transfer.
 *
 * The synchronous portable frontend calls under its bus mutex, with software
 * CS asserted. Async worker adapters may reuse this function without that
 * mutex after async admission. @p tx or @p rx may be NULL for half-duplex.
 * Count bytes, converting to whole native frames where needed; exact in-place
 * TX/RX is supported. The frontend has checked frame/alignment/overlap constraints
 * and backend limits; the caller supplies the full valid memory spans.
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
 * result when I/O finishes. This records the result and deasserts software
 * CS. Admission remains held until the submission/provider rendezvous and
 * final task-context retirement; recording a result does not notify users.
 *
 * Backends without native DMA support may implement this via a
 * worker thread (NuttX), a system-workqueue operation (Zephyr), or return
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

/** Establish task-context completion dispatch before accepting async work.
 * Called from thread context. Failure must leave no operation retained.
 */
int ove_hal_spi_async_prepare(void);

/** Retire a completed operation after the common submission rendezvous.
 * The provider must dispatch ove_spi_async_retired() in task context after
 * its final controller access, including embedded kernel-work tails. Queues
 * must unlink/cache every controller reference first. After notifying, the
 * controller may have been destroyed; no further access is permitted.
 */
void ove_hal_spi_async_retire(ove_spi_t spi);

/** Final task-context notification; provider retention has ended.
 * Internal to HAL implementations. Calls the cached user callback and may
 * finish an already requested close. No provider may access spi afterward.
 */
void ove_spi_async_retired(ove_spi_t spi);

/**
 * @brief Completion notification from the HAL.
 *
 * Called by the HAL backend (from worker thread or DMA ISR) when an
 * async I/O finishes. The portable layer deasserts CS and records the result.
 * Its atomic submission/result rendezvous transfers exactly one retirement
 * request to the provider hook. That hook must establish the provider-tail
 * barrier before final task notification; busy is not cleared here.
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
