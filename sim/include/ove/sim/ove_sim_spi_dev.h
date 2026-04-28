/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_spi_dev Simulation Virtual SPI Device
 * @brief Sub-interface for plugins that emulate SPI slave devices.
 *
 * The sim SPI HAL (sim_spi.c) routes bus transactions to registered
 * virtual device plugins based on bus instance and CS pin.
 * @{
 */

#ifndef OVE_SIM_SPI_DEV_H
#define OVE_SIM_SPI_DEV_H

#include "ove_sim_plugin.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of registered virtual SPI devices. */
#define OVE_SIM_SPI_MAX_DEVICES 8

/**
 * @brief Extended ops for virtual SPI device plugins.
 */
struct ove_sim_spi_dev_ops {
	struct ove_sim_plugin_ops base;

	unsigned int bus;     /**< SPI bus instance index. */
	unsigned int cs_port; /**< CS GPIO port. */
	unsigned int cs_pin;  /**< CS GPIO pin. */

	/**
	 * @brief Handle a full-duplex SPI transfer.
	 *
	 * @param[in]  ctx     Plugin-private context.
	 * @param[in]  tx      TX data (MOSI), or NULL for read-only.
	 * @param[out] rx      RX data (MISO), or NULL for write-only.
	 * @param[in]  len     Transfer length in bytes.
	 * @return 0 on success, negative on error.
	 */
	int (*transfer)(void *ctx, const void *tx, void *rx, size_t len);
};

/**
 * @brief Register a virtual SPI device plugin.
 *
 * @param[in] ops  Extended ops including bus/CS and transfer callback.
 * @param[in] ctx  Plugin-private context.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_spi_dev_register(const struct ove_sim_spi_dev_ops *ops, void *ctx);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_SPI_DEV_H */
