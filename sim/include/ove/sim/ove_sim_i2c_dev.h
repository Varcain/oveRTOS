/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_i2c_dev Simulation Virtual I2C Device
 * @brief Sub-interface for plugins that emulate I2C slave devices.
 *
 * The sim I2C HAL (sim_i2c.c) routes bus transactions to registered
 * virtual device plugins based on bus instance and device address.
 * @{
 */

#ifndef OVE_SIM_I2C_DEV_H
#define OVE_SIM_I2C_DEV_H

#include "ove_sim_plugin.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of registered virtual I2C devices. */
#define OVE_SIM_I2C_MAX_DEVICES 16

/**
 * @brief Extended ops for virtual I2C device plugins.
 *
 * Inherits the base plugin ops and adds bus-specific callbacks.
 */
struct ove_sim_i2c_dev_ops {
	struct ove_sim_plugin_ops base;

	uint16_t addr;	  /**< 7-bit I2C address this device responds to. */
	unsigned int bus; /**< I2C bus instance index. */

	/**
	 * @brief Handle an I2C write (master -> slave).
	 *
	 * @param[in] ctx   Plugin-private context.
	 * @param[in] data  Write data (first byte is typically register address).
	 * @param[in] len   Number of bytes.
	 * @return 0 on success (ACK), negative on error (NACK).
	 */
	int (*write)(void *ctx, const void *data, size_t len);

	/**
	 * @brief Handle an I2C read (slave -> master).
	 *
	 * @param[in]  ctx  Plugin-private context.
	 * @param[out] buf  Buffer to fill with response data.
	 * @param[in]  len  Number of bytes requested.
	 * @return 0 on success, negative on error.
	 */
	int (*read)(void *ctx, void *buf, size_t len);

	/**
	 * @brief Handle an I2C write-then-read transaction.
	 *
	 * @param[in]  ctx     Plugin-private context.
	 * @param[in]  tx      TX data (register address + optional data).
	 * @param[in]  tx_len  TX byte count.
	 * @param[out] rx      RX buffer.
	 * @param[in]  rx_len  RX byte count.
	 * @return 0 on success, negative on error.
	 */
	int (*write_read)(void *ctx, const void *tx, size_t tx_len, void *rx, size_t rx_len);
};

/**
 * @brief Register a virtual I2C device plugin.
 *
 * @param[in] ops  Extended ops including bus address and callbacks.
 * @param[in] ctx  Plugin-private context.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_i2c_dev_register(const struct ove_sim_i2c_dev_ops *ops, void *ctx);

/**
 * @brief Find a registered virtual I2C device by bus and address.
 *
 * @param[in] bus   I2C bus instance index.
 * @param[in] addr  7-bit device address.
 * @return Pointer to the device entry, or NULL if none registered.
 */
const struct ove_sim_i2c_dev_ops *ove_sim_i2c_dev_find(unsigned int bus, uint16_t addr);

/**
 * @brief Get the context for a registered I2C device.
 *
 * @param[in] bus   I2C bus instance index.
 * @param[in] addr  7-bit device address.
 * @return Plugin-private context, or NULL if not found.
 */
void *ove_sim_i2c_dev_get_ctx(unsigned int bus, uint16_t addr);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_I2C_DEV_H */
