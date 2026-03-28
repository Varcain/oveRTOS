/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef INC_I2S_STM32F7_H_
#define INC_I2S_STM32F7_H_

#include "i2s_da.h"

void i2s_stm32f7_init(void);

/**
 * @brief Set audio input device before init.
 * @param use_dmic  Non-zero to use on-board digital microphones,
 *                  zero for line-in (default).
 */
void i2s_stm32f7_set_input_device(int use_dmic);

/**
 * @brief Override the sample rate before init.
 * @param rate  Sample rate in Hz (e.g. 16000, 44100, 48000).
 *              0 means use the board default.
 */
void i2s_stm32f7_set_sample_rate(unsigned int rate);
unsigned long i2s_stm32f7_getRxBuffer(void);
unsigned long i2s_stm32f7_getTxBuffer(void);
uint8_t i2s_stm32f7_getRxCompletedBufferHalf(void);
uint8_t i2s_stm32f7_getTxCompletedBufferHalf(void);
unsigned long i2s_stm32f7_getRxBufferFirstHalf(void);
unsigned long i2s_stm32f7_getRxBufferSecondHalf(void);
unsigned long i2s_stm32f7_getTxBufferFirstHalf(void);
unsigned long i2s_stm32f7_getTxBufferSecondHalf(void);
int i2s_stm32f7_rxBufferRdy(void);
unsigned int i2s_stm32f7_rx_xferCnt(void);
unsigned int i2s_stm32f7_tx_xferCnt(void);
void i2s_stm32f7_setRxCompleteCb(i2s_driver_rxCompleteCb cb);
void i2s_stm32f7_setTxCompleteCb(i2s_driver_rxCompleteCb cb);
void i2s_stm32f7_startStream(void);
void i2s_stm32f7_stopStream(void);
void i2s_stm32f7_pauseStream(void);
void i2s_stm32f7_resumeStream(void);
void i2s_stm32f7_getRxDebugCounters(uint32_t *half_count, uint32_t *full_count);
void i2s_stm32f7_getTxDebugCounters(uint32_t *half_count, uint32_t *full_count);
uint32_t i2s_stm32f7_getRxCallbackCount(void);
uint32_t i2s_stm32f7_getTxCallbackCount(void);
uint32_t i2s_stm32f7_getPhaseAlignment(void);

struct i2s_drv_ops* i2s_stm32f7_get(void);

#endif /* INC_I2S_STM32F7_H_ */
