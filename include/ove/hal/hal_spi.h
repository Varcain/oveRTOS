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
int  ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg);

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
 * @param[in]  timeout_ms Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx,
			  size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_SPI_H */
