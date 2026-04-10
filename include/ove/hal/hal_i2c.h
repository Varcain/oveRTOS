/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_I2C_H
#define OVE_HAL_I2C_H

/**
 * @defgroup ove_hal_i2c HAL I2C Interface
 * @brief Hardware Abstraction Layer interface for I2C bus operations.
 *
 * Declares the low-level I2C functions that every platform HAL must
 * implement.  The portable @ref ove_i2c layer delegates to these
 * functions after performing parameter validation and bus locking.
 * @{
 */

#include "ove/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure and enable the I2C peripheral.
 *
 * Called by ove_i2c_init() after validation.  The HAL must map the
 * instance index to the correct hardware peripheral, configure the
 * clock speed, and enable the I2C controller.
 *
 * @param[in] i2c  I2C handle with storage already assigned.
 * @param[in] cfg  Bus configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg);

/**
 * @brief Disable and release the I2C peripheral.
 *
 * Called by ove_i2c_deinit().
 *
 * @param[in] i2c  I2C handle.
 */
void ove_hal_i2c_close(ove_i2c_t i2c);

/**
 * @brief Write data to an I2C device.
 *
 * Called under the bus mutex.
 *
 * @param[in] i2c        I2C handle.
 * @param[in] addr       7-bit device address.
 * @param[in] data       Data to transmit.
 * @param[in] len        Number of bytes.
 * @param[in] timeout_ms Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr,
		       const void *data, size_t len,
		       uint32_t timeout_ms);

/**
 * @brief Read data from an I2C device.
 *
 * Called under the bus mutex.
 *
 * @param[in]  i2c        I2C handle.
 * @param[in]  addr       7-bit device address.
 * @param[out] buf        Buffer to receive data.
 * @param[in]  len        Number of bytes.
 * @param[in]  timeout_ms Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr,
		      void *buf, size_t len, uint32_t timeout_ms);

/**
 * @brief Combined write-then-read with repeated start.
 *
 * Called under the bus mutex.  All four backends support this
 * atomically (STM32 HAL_I2C_Mem_Read, Zephyr i2c_write_read,
 * NuttX I2C_TRANSFER, POSIX I2C_RDWR).
 *
 * @param[in]  i2c        I2C handle.
 * @param[in]  addr       7-bit device address.
 * @param[in]  tx         Transmit buffer.
 * @param[in]  tx_len     Number of bytes to write.
 * @param[out] rx         Receive buffer.
 * @param[in]  rx_len     Number of bytes to read.
 * @param[in]  timeout_ms Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr,
			    const void *tx, size_t tx_len,
			    void *rx, size_t rx_len,
			    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_I2C_H */
