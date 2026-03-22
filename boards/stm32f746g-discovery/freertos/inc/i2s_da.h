/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef _I2S_DA_H_
#define _I2S_DA_H_

#include <stdint.h>

/* Forward declaration */
struct i2s_drv_ops;

/* Callback type for RX completion notification */
typedef void (*i2s_driver_rxCompleteCb)(void);

/* I2S driver operations structure */
struct i2s_drv_ops {
	/* Initialize the I2S hardware */
	void (*init)(void);

	/* Get pointer to RX buffer (returns address as unsigned long) */
	unsigned long (*getRxBuffer)(void);

	/* Get pointer to TX buffer (returns address as unsigned long) */
	unsigned long (*getTxBuffer)(void);

	/* Check if RX buffer is ready (returns 1 if ready, 0 otherwise) */
	int (*rxBufferRdy)(void);

	/* Get transfer count (remaining bytes in DMA transfer) */
	unsigned int (*xferCnt)(void);

	/* Set RX completion callback */
	void (*setRxCompleteCb)(i2s_driver_rxCompleteCb cb);

	/* Start I2S stream */
	void (*startStream)(void);

	/* Pause I2S stream */
	void (*pauseStream)(void);

	/* Resume I2S stream */
	void (*resumeStream)(void);
};

/* Public API */

/**
 * @brief Set the I2S driver implementation
 * @param ops Pointer to driver operations structure (must not be NULL)
 * @return 1 if driver was set successfully, 0 otherwise
 */
int i2s_set_driver(struct i2s_drv_ops *ops);

/**
 * @brief Check if I2S driver is initialized
 * @return 1 if driver is set and ready, 0 otherwise
 */
int i2s_is_initialized(void);

/**
 * @brief Initialize I2S hardware
 * @note Must call i2s_set_driver() before this function
 */
void i2s_init(void);

/**
 * @brief Get pointer to RX buffer
 * @return Address of RX buffer, or 0 if driver not initialized
 */
unsigned long i2s_getRxBuffer(void);

/**
 * @brief Get pointer to TX buffer
 * @return Address of TX buffer, or 0 if driver not initialized
 */
unsigned long i2s_getTxBuffer(void);

/**
 * @brief Check if RX buffer is ready
 * @return 1 if ready, 0 if not ready, -1 if driver not initialized
 */
int i2s_rxBufferRdy(void);

/**
 * @brief Get transfer count (remaining bytes in DMA)
 * @return Transfer count, or 0 if driver not initialized
 */
unsigned int i2s_xferCnt(void);

/**
 * @brief Set RX completion callback
 * @param cb Callback function to call when RX buffer is ready
 */
void i2s_setRxCompleteCb(i2s_driver_rxCompleteCb cb);

/**
 * @brief Start I2S stream
 */
void i2s_startStream(void);

/**
 * @brief Pause I2S stream
 */
void i2s_pauseStream(void);

/**
 * @brief Resume I2S stream
 */
void i2s_resumeStream(void);

#endif /* _I2S_DA_H_ */
