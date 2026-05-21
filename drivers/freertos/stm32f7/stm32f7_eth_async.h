/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * STM32F7 Ethernet transport for the embassy-net driver. Mirrors
 * qemu_net_async.h's signature so the Rust QemuShmDriver pattern
 * applies — the Stm32f7EthDriver just swaps the C entry points.
 *
 * Mutually exclusive with the lwIP-based stm32f7_eth.c at build time
 * (both define `heth` and `HAL_ETH_Rx*Callback`); choice via
 * CONFIG_OVE_NET vs CONFIG_OVE_ASYNC_NET.
 */

#ifndef OVE_STM32F7_ETH_ASYNC_H
#define OVE_STM32F7_ETH_ASYNC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the STM32F7 ETH MAC + LAN8742A PHY for embassy-net.
 *
 * Sets the MAC address, runs PHY autoneg (up to 5 s), drives MAC
 * speed/duplex from the negotiated result, and starts the DMA. Spawns
 * the RMII errata watchdog on STM32F746 rev A silicon.
 *
 * @param[in] mac  6-byte MAC address.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_stm32f7_eth_async_init(const uint8_t mac[6]);

/**
 * @brief Send one Ethernet frame.
 *
 * Blocking until the DMA accepts the descriptor (≤100 ms). Returns
 * @c OVE_ERR_BUS_ERROR if the MAC rejects the transfer.
 */
int ove_stm32f7_eth_async_tx(const void *frame, uint32_t len);

/**
 * @brief Try to dequeue one received frame.
 *
 * Drives @c HAL_ETH_ReadData to advance the DMA; if a complete frame
 * is available it is copied into @p buf and the slot is returned to
 * the descriptor pool.
 *
 * @return OVE_OK if a frame was produced, @c OVE_ERR_NOT_FOUND if
 *         empty, negative on error.
 */
int ove_stm32f7_eth_async_rx(void *buf, uint32_t buf_size, uint32_t *out_len);

/**
 * @brief Read the PHY link status (cached at init + refreshed on demand).
 */
int ove_stm32f7_eth_async_link_up(void);

#ifdef __cplusplus
}
#endif

#endif /* OVE_STM32F7_ETH_ASYNC_H */
