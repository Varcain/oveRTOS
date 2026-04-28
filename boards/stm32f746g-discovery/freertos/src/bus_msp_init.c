/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746G-Discovery board-specific MSP init callbacks for bus peripherals.
 * These functions are called by the STM32 HAL during HAL_UART_Init(),
 * HAL_SPI_Init(), and HAL_I2C_Init() to configure GPIO pins and clocks.
 */

#include "ove_config.h"
#include "stm32f7xx_hal.h"

/* ── UART MSP ────────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_UART

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef gpio = {0};

	if (huart->Instance == USART1) {
		/* USART1: PA9 (TX), PB7 (RX) — ST-Link VCP */
		__HAL_RCC_USART1_CLK_ENABLE();
		__HAL_RCC_GPIOA_CLK_ENABLE();
		__HAL_RCC_GPIOB_CLK_ENABLE();

		gpio.Pin = GPIO_PIN_9;
		gpio.Mode = GPIO_MODE_AF_PP;
		gpio.Pull = GPIO_PULLUP;
		gpio.Speed = GPIO_SPEED_FREQ_HIGH;
		gpio.Alternate = GPIO_AF7_USART1;
		HAL_GPIO_Init(GPIOA, &gpio);

		gpio.Pin = GPIO_PIN_7;
		gpio.Alternate = GPIO_AF7_USART1;
		HAL_GPIO_Init(GPIOB, &gpio);
	} else if (huart->Instance == USART6) {
		/* USART6: PC6 (TX), PC7 (RX) — Arduino header */
		__HAL_RCC_USART6_CLK_ENABLE();
		__HAL_RCC_GPIOC_CLK_ENABLE();

		gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
		gpio.Mode = GPIO_MODE_AF_PP;
		gpio.Pull = GPIO_PULLUP;
		gpio.Speed = GPIO_SPEED_FREQ_HIGH;
		gpio.Alternate = GPIO_AF8_USART6;
		HAL_GPIO_Init(GPIOC, &gpio);
	}
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
	if (huart->Instance == USART1) {
		__HAL_RCC_USART1_CLK_DISABLE();
		HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9);
		HAL_GPIO_DeInit(GPIOB, GPIO_PIN_7);
	} else if (huart->Instance == USART6) {
		__HAL_RCC_USART6_CLK_DISABLE();
		HAL_GPIO_DeInit(GPIOC, GPIO_PIN_6 | GPIO_PIN_7);
	}
}

#endif /* CONFIG_OVE_UART */

/* ── SPI MSP ─────────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_SPI

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
	GPIO_InitTypeDef gpio = {0};

	if (hspi->Instance == SPI2) {
		/* SPI2: PI1 (SCK), PB14 (MISO), PB15 (MOSI) — Arduino header */
		__HAL_RCC_SPI2_CLK_ENABLE();
		__HAL_RCC_GPIOI_CLK_ENABLE();
		__HAL_RCC_GPIOB_CLK_ENABLE();

		gpio.Pin = GPIO_PIN_1;
		gpio.Mode = GPIO_MODE_AF_PP;
		gpio.Pull = GPIO_NOPULL;
		gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
		gpio.Alternate = GPIO_AF5_SPI2;
		HAL_GPIO_Init(GPIOI, &gpio);

		gpio.Pin = GPIO_PIN_14 | GPIO_PIN_15;
		gpio.Alternate = GPIO_AF5_SPI2;
		HAL_GPIO_Init(GPIOB, &gpio);
	}
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef *hspi)
{
	if (hspi->Instance == SPI2) {
		__HAL_RCC_SPI2_CLK_DISABLE();
		HAL_GPIO_DeInit(GPIOI, GPIO_PIN_1);
		HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14 | GPIO_PIN_15);
	}
}

#endif /* CONFIG_OVE_SPI */

/* ── I2C MSP ─────────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_I2C

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
	GPIO_InitTypeDef gpio = {0};

	if (hi2c->Instance == I2C1) {
		/* I2C1: PB8 (SCL), PB9 (SDA) — Arduino header & audio codec */
		__HAL_RCC_I2C1_CLK_ENABLE();
		__HAL_RCC_GPIOB_CLK_ENABLE();

		gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
		gpio.Mode = GPIO_MODE_AF_OD;
		gpio.Pull = GPIO_PULLUP;
		gpio.Speed = GPIO_SPEED_FREQ_HIGH;
		gpio.Alternate = GPIO_AF4_I2C1;
		HAL_GPIO_Init(GPIOB, &gpio);
	} else if (hi2c->Instance == I2C3) {
		/* I2C3: PH7 (SCL), PH8 (SDA) — touch controller */
		__HAL_RCC_I2C3_CLK_ENABLE();
		__HAL_RCC_GPIOH_CLK_ENABLE();

		gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8;
		gpio.Mode = GPIO_MODE_AF_OD;
		gpio.Pull = GPIO_PULLUP;
		gpio.Speed = GPIO_SPEED_FREQ_HIGH;
		gpio.Alternate = GPIO_AF4_I2C3;
		HAL_GPIO_Init(GPIOH, &gpio);
	}
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef *hi2c)
{
	if (hi2c->Instance == I2C1) {
		__HAL_RCC_I2C1_CLK_DISABLE();
		HAL_GPIO_DeInit(GPIOB, GPIO_PIN_8 | GPIO_PIN_9);
	} else if (hi2c->Instance == I2C3) {
		__HAL_RCC_I2C3_CLK_DISABLE();
		HAL_GPIO_DeInit(GPIOH, GPIO_PIN_7 | GPIO_PIN_8);
	}
}

#endif /* CONFIG_OVE_I2C */

/* ── ETH MSP ─────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_NET

/*
 * STM32F746G-Discovery RMII pin mapping:
 *   PA1  RMII_REF_CLK    PA2  RMII_MDIO     PA7  RMII_CRS_DV
 *   PC1  RMII_MDC        PC4  RMII_RXD0     PC5  RMII_RXD1
 *   PG11 RMII_TX_EN      PG13 RMII_TXD0     PG14 RMII_TXD1
 */
void HAL_ETH_MspInit(ETH_HandleTypeDef *h)
{
	GPIO_InitTypeDef gpio = {0};
	(void)h;

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();

	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = GPIO_AF11_ETH;

	gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
	HAL_GPIO_Init(GPIOA, &gpio);

	gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
	HAL_GPIO_Init(GPIOC, &gpio);

	gpio.Pin = GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14;
	HAL_GPIO_Init(GPIOG, &gpio);

	__HAL_RCC_ETH_CLK_ENABLE();
}

#endif /* CONFIG_OVE_NET */

/* ── SAI / I2S MSP + codec init ──────────────────────────────── */

#ifdef CONFIG_OVE_I2S

#include "ove/i2s.h"
#include "stm32746g_discovery_audio.h"

/* Access the I2S handle for DMA IRQ forwarding */
extern ove_i2s_t g_i2s_instance;

/*
 * HAL_SAI_MspInit — called automatically by HAL_SAI_Init().
 * Configures GPIO, DMA, clocks, and NVIC for SAI2 on the Discovery board.
 *
 * SAI2 Block A (Master TX): PI4=SCK, PI5=SD, PI6=MCLK, PI7=FS
 * SAI2 Block B (Slave RX):  PG10=SD
 */
void HAL_SAI_MspInit(SAI_HandleTypeDef *hsai)
{
	GPIO_InitTypeDef gpio = {0};

	/* Enable clocks */
	AUDIO_OUT_SAIx_CLK_ENABLE();
	AUDIO_IN_SAIx_CLK_ENABLE();
	AUDIO_OUT_SAIx_MCLK_ENABLE();
	AUDIO_OUT_SAIx_SCK_SD_ENABLE();
	AUDIO_OUT_SAIx_FS_ENABLE();
	AUDIO_IN_SAIx_SD_ENABLE();
	AUDIO_IN_INT_GPIO_ENABLE();
	AUDIO_OUT_SAIx_DMAx_CLK_ENABLE();
	AUDIO_IN_SAIx_DMAx_CLK_ENABLE();

	/* TX GPIO: FS, SCK, SD, MCLK */
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_HIGH;

	gpio.Pin = AUDIO_OUT_SAIx_FS_PIN;
	gpio.Alternate = AUDIO_OUT_SAIx_FS_SD_MCLK_AF;
	HAL_GPIO_Init(AUDIO_OUT_SAIx_FS_GPIO_PORT, &gpio);

	gpio.Pin = AUDIO_OUT_SAIx_SCK_PIN;
	gpio.Alternate = AUDIO_OUT_SAIx_SCK_AF;
	HAL_GPIO_Init(AUDIO_OUT_SAIx_SCK_SD_GPIO_PORT, &gpio);

	gpio.Pin = AUDIO_OUT_SAIx_SD_PIN;
	gpio.Alternate = AUDIO_OUT_SAIx_FS_SD_MCLK_AF;
	HAL_GPIO_Init(AUDIO_OUT_SAIx_SCK_SD_GPIO_PORT, &gpio);

	gpio.Pin = AUDIO_OUT_SAIx_MCLK_PIN;
	gpio.Alternate = AUDIO_OUT_SAIx_FS_SD_MCLK_AF;
	HAL_GPIO_Init(AUDIO_OUT_SAIx_MCLK_GPIO_PORT, &gpio);

	/* RX GPIO: SD */
	gpio.Pin = AUDIO_IN_SAIx_SD_PIN;
	gpio.Speed = GPIO_SPEED_FAST;
	gpio.Alternate = AUDIO_IN_SAIx_SD_AF;
	HAL_GPIO_Init(AUDIO_IN_SAIx_SD_GPIO_PORT, &gpio);

	/* Audio interrupt pin (codec) */
	gpio.Pin = AUDIO_IN_INT_GPIO_PIN;
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FAST;
	HAL_GPIO_Init(AUDIO_IN_INT_GPIO_PORT, &gpio);

	/* PLLI2S clock for 44.1 kHz audio */
	RCC_PeriphCLKInitTypeDef rcc = {0};
	HAL_RCCEx_GetPeriphCLKConfig(&rcc);
	rcc.PeriphClockSelection = RCC_PERIPHCLK_SAI2;
	rcc.Sai2ClockSelection = RCC_SAI2CLKSOURCE_PLLI2S;
	rcc.PLLI2S.PLLI2SN = 429;
	rcc.PLLI2S.PLLI2SQ = 2;
	rcc.PLLI2SDivQ = 19;
	HAL_RCCEx_PeriphCLKConfig(&rcc);

	/* DMA for TX (SAI2 Block A) */
	if (hsai->Instance == AUDIO_OUT_SAIx && g_i2s_instance) {
		DMA_HandleTypeDef *dma_tx = &g_i2s_instance->dma_tx;
		dma_tx->Instance = AUDIO_OUT_SAIx_DMAx_STREAM;
		dma_tx->Init.Channel = AUDIO_OUT_SAIx_DMAx_CHANNEL;
		dma_tx->Init.Direction = DMA_MEMORY_TO_PERIPH;
		dma_tx->Init.PeriphInc = DMA_PINC_DISABLE;
		dma_tx->Init.MemInc = DMA_MINC_ENABLE;
		dma_tx->Init.PeriphDataAlignment = AUDIO_OUT_SAIx_DMAx_PERIPH_DATA_SIZE;
		dma_tx->Init.MemDataAlignment = AUDIO_OUT_SAIx_DMAx_MEM_DATA_SIZE;
		dma_tx->Init.Mode = DMA_CIRCULAR;
		dma_tx->Init.Priority = DMA_PRIORITY_HIGH;
		dma_tx->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
		dma_tx->Init.MemBurst = DMA_MBURST_SINGLE;
		dma_tx->Init.PeriphBurst = DMA_PBURST_SINGLE;
		__HAL_LINKDMA(hsai, hdmatx, *dma_tx);
		HAL_DMA_DeInit(dma_tx);
		HAL_DMA_Init(dma_tx);
		HAL_NVIC_SetPriority(AUDIO_OUT_SAIx_DMAx_IRQ, 6, 0);
		HAL_NVIC_EnableIRQ(AUDIO_OUT_SAIx_DMAx_IRQ);
	}

	/* DMA for RX (SAI2 Block B) */
	if (hsai->Instance == AUDIO_IN_SAIx && g_i2s_instance) {
		DMA_HandleTypeDef *dma_rx = &g_i2s_instance->dma_rx;
		dma_rx->Instance = AUDIO_IN_SAIx_DMAx_STREAM;
		dma_rx->Init.Channel = AUDIO_IN_SAIx_DMAx_CHANNEL;
		dma_rx->Init.Direction = DMA_PERIPH_TO_MEMORY;
		dma_rx->Init.PeriphInc = DMA_PINC_DISABLE;
		dma_rx->Init.MemInc = DMA_MINC_ENABLE;
		dma_rx->Init.PeriphDataAlignment = AUDIO_IN_SAIx_DMAx_PERIPH_DATA_SIZE;
		dma_rx->Init.MemDataAlignment = AUDIO_IN_SAIx_DMAx_MEM_DATA_SIZE;
		dma_rx->Init.Mode = DMA_CIRCULAR;
		dma_rx->Init.Priority = DMA_PRIORITY_HIGH;
		dma_rx->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
		dma_rx->Init.MemBurst = DMA_MBURST_SINGLE;
		dma_rx->Init.PeriphBurst = DMA_MBURST_SINGLE;
		__HAL_LINKDMA(hsai, hdmarx, *dma_rx);
		HAL_DMA_DeInit(dma_rx);
		HAL_DMA_Init(dma_rx);
		HAL_NVIC_SetPriority(AUDIO_IN_SAIx_DMAx_IRQ, 6, 0);
		HAL_NVIC_EnableIRQ(AUDIO_IN_SAIx_DMAx_IRQ);
	}

	HAL_NVIC_SetPriority(AUDIO_IN_INT_IRQ, 6, 0);
	HAL_NVIC_EnableIRQ(AUDIO_IN_INT_IRQ);
}

/* DMA IRQ handlers — forward to HAL which invokes SAI callbacks */
void AUDIO_OUT_SAIx_DMAx_IRQHandler(void)
{
	if (g_i2s_instance)
		HAL_DMA_IRQHandler(g_i2s_instance->sai_tx.hdmatx);
}

void AUDIO_IN_SAIx_DMAx_IRQHandler(void)
{
	if (g_i2s_instance)
		HAL_DMA_IRQHandler(g_i2s_instance->sai_rx.hdmarx);
}

/*
 * Board-specific codec init for STM32F746G-Discovery (WM8994).
 * Overrides the weak default in freertos_audio.c.
 */
void ove_board_audio_codec_init(uint32_t sample_rate, int input_device)
{
	uint32_t codec_id = wm8994_drv.ReadID(AUDIO_I2C_ADDRESS);
	if (codec_id != WM8994_ID)
		return;

	wm8994_drv.Reset(AUDIO_I2C_ADDRESS);

	uint16_t input_dev = input_device ? INPUT_DEVICE_DIGITAL_MICROPHONE_2
					  : INPUT_DEVICE_INPUT_LINE_1;

	wm8994_drv.Init(AUDIO_I2C_ADDRESS, input_dev | OUTPUT_DEVICE_HEADPHONE, 70, sample_rate);

	/* Override BSP defaults — must match reference order exactly */

	/* Power Management 1: disable speaker output amps */
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x01, 0x0313);

	/* Power Management 3: disable speaker mixer amps */
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x03, 0x0030);

	/* Input mixer: remove output-to-input feedback and +30dB boost */
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x29, 0x0020);
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x2A, 0x0020);

	/* Speaker Mixer: disconnect DAC2 */
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x36, 0x0000);

	/* Output Mixer 1 & 2: route DAC1 to headphone output */
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x2D, 0x0100);
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x2E, 0x0100);

	/* HP volume: +1dB */
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x1C, 0x01FA);
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x1D, 0x01FA);

	/* ADC digital volume: 0dB */
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x400, 0x01C0);
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x401, 0x01C0);

	if (input_device) {
		/* DMIC mode overrides */
		AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x420, 0x0000);
		AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x404, 0x01D8);
		AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x405, 0x01D8);
	}

	/* Enable oversampling for better SNR */
	AUDIO_IO_Write(AUDIO_I2C_ADDRESS, 0x620, 0x0002);
}

#endif /* CONFIG_OVE_I2S */
