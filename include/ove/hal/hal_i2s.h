/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_I2S_H
#define OVE_HAL_I2S_H

/**
 * @defgroup ove_hal_i2s HAL I2S Interface
 * @brief Hardware Abstraction Layer for I2S / SAI bus operations.
 *
 * The HAL configures the I2S peripheral and manages DMA streaming.
 * Codec initialisation is NOT part of this interface — it is
 * board-specific and done separately via I2C.
 *
 * GPIO and clock setup is handled by MCU HAL weak callbacks
 * (e.g. HAL_SAI_MspInit on STM32) provided by the board BSP.
 *
 * DMA half/full-complete ISRs must call the portable helpers:
 *   ove_i2s_rx_half_cplt_isr(), ove_i2s_rx_cplt_isr()
 *   ove_i2s_tx_half_cplt_isr(), ove_i2s_tx_cplt_isr()
 * @{
 */

#include "ove/i2s.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure and enable the I2S / SAI peripheral.
 *
 * Maps the instance index to hardware, configures sample rate and
 * framing, and sets up DMA channels.  Does NOT start streaming.
 *
 * @param[in] i2s  I2S handle with storage already assigned.
 * @param[in] cfg  Configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_hal_i2s_open(ove_i2s_t i2s, const struct ove_i2s_cfg *cfg);

/**
 * @brief Disable the I2S peripheral and release DMA resources.
 */
void ove_hal_i2s_close(ove_i2s_t i2s);

/**
 * @brief Start circular DMA streaming.
 *
 * TX starts first (generates clocks for synchronous RX slave).
 */
int  ove_hal_i2s_start(ove_i2s_t i2s);

/**
 * @brief Stop DMA streaming.
 */
int  ove_hal_i2s_stop(ove_i2s_t i2s);

/**
 * @brief Pause DMA (can be resumed without reconfiguration).
 */
int  ove_hal_i2s_pause(ove_i2s_t i2s);

/**
 * @brief Resume DMA after pause.
 */
int  ove_hal_i2s_resume(ove_i2s_t i2s);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_I2S_H */
