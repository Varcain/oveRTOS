/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F7 SAI / I2S bus driver.
 *
 * Configures the SAI peripheral and DMA for circular audio streaming.
 * GPIO, clock, and DMA channel setup is handled by HAL_SAI_MspInit()
 * which must be provided by the board BSP.
 *
 * Codec initialisation is NOT done here — it is board-specific and
 * should be performed separately via I2C register writes.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_I2S

#include "ove/hal/hal_i2s.h"
#include "ove_backend_common.h"
#include "stm32f7xx_hal.h"

#include <string.h>

/* ISR helpers from portable layer */
extern void ove_i2s_rx_half_cplt_isr(ove_i2s_t i2s);
extern void ove_i2s_rx_cplt_isr(ove_i2s_t i2s);
extern void ove_i2s_tx_half_cplt_isr(ove_i2s_t i2s);
extern void ove_i2s_tx_cplt_isr(ove_i2s_t i2s);

/* Single-instance dispatch — ISR callbacks need access to the handle */
ove_i2s_t g_i2s_instance;

/* ── SAI instance mapping ────────────────────────────────────── */

static SAI_Block_TypeDef *instance_to_sai_tx(unsigned int instance)
{
	switch (instance) {
	case 0:
		return SAI1_Block_A;
	case 1:
		return SAI2_Block_A;
	default:
		return NULL;
	}
}

static SAI_Block_TypeDef *instance_to_sai_rx(unsigned int instance)
{
	switch (instance) {
	case 0:
		return SAI1_Block_B;
	case 1:
		return SAI2_Block_B;
	default:
		return NULL;
	}
}

/* ── HAL implementation ──────────────────────────────────────── */

int ove_hal_i2s_open(ove_i2s_t i2s, const struct ove_i2s_cfg *cfg)
{
	SAI_Block_TypeDef *sai_tx_block = instance_to_sai_tx(cfg->instance);
	SAI_Block_TypeDef *sai_rx_block = instance_to_sai_rx(cfg->instance);

	if (sai_tx_block == NULL)
		return OVE_ERR_INVALID_PARAM;

	g_i2s_instance = i2s;

	uint32_t data_size;
	uint32_t frame_length;
	uint32_t active_frame_length;

	switch (cfg->bit_depth) {
	case 32:
		data_size = SAI_DATASIZE_32;
		frame_length = 64;
		active_frame_length = 32;
		break;
	case 24:
		data_size = SAI_DATASIZE_24;
		frame_length = 64;
		active_frame_length = 32;
		break;
	default: /* 16 — 4 slots × 16 bits = 64-bit frame */
		data_size = SAI_DATASIZE_16;
		frame_length = 64;
		active_frame_length = 32;
		break;
	}

	/* 4-slot 64-bit frame matches the WM8994's AIF1 layout used by the
	 * STM32CubeF7 BSP_AUDIO_IN reference (CODEC_AUDIOFRAME_SLOT_0123).
	 * Within each LRCLK half, the codec multiplexes two AIF1 timeslots:
	 *   slot 0 / slot 2 = AIF1 Timeslot 0 L / R (DAC1 in, line-in/ADC1 out)
	 *   slot 1 / slot 3 = AIF1 Timeslot 1 L / R (DMIC2 out)
	 * With 2-slot 32-bit framing the codec's frame sync wouldn't align and
	 * the DMIC routing (reg 0x608/0x609 → AIF1ADC2 Timeslot 1) had no path
	 * onto the SAI bus.  `freertos_audio.c` picks which slots to read or
	 * write based on `cfg->i2s.input_device`.  We do *not* enable
	 * SAI_MONOMODE for mono channels: the audio source/sink in
	 * `freertos_audio.c` always reads/writes with a stride of `slot_count`,
	 * so every frame must contain that many samples on the bus. */
	uint32_t slot_count = 4;
	uint32_t slot_active = SAI_SLOTACTIVE_0 | SAI_SLOTACTIVE_1 | SAI_SLOTACTIVE_2 |
			       SAI_SLOTACTIVE_3;

	/* Configure TX SAI (master) */
	if (cfg->direction & OVE_I2S_DIR_TX) {
		memset(&i2s->sai_tx, 0, sizeof(i2s->sai_tx));
		i2s->sai_tx.Instance = sai_tx_block;
		i2s->sai_tx.Init.AudioFrequency = cfg->sample_rate;
		i2s->sai_tx.Init.AudioMode = SAI_MODEMASTER_TX;
		i2s->sai_tx.Init.NoDivider = SAI_MASTERDIVIDER_ENABLED;
		i2s->sai_tx.Init.Protocol = SAI_FREE_PROTOCOL;
		i2s->sai_tx.Init.DataSize = data_size;
		i2s->sai_tx.Init.FirstBit = SAI_FIRSTBIT_MSB;
		i2s->sai_tx.Init.ClockStrobing = SAI_CLOCKSTROBING_FALLINGEDGE;
		i2s->sai_tx.Init.Synchro = SAI_ASYNCHRONOUS;
		i2s->sai_tx.Init.OutputDrive = SAI_OUTPUTDRIVE_ENABLED;
		i2s->sai_tx.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_FULL;
		/* MonoStereoMode left at default SAI_STEREOMODE — see comment
		 * above; the source/sink rely on a stereo DMA buffer layout. */

		i2s->sai_tx.FrameInit.FrameLength = frame_length;
		i2s->sai_tx.FrameInit.ActiveFrameLength = active_frame_length;
		i2s->sai_tx.FrameInit.FSDefinition = SAI_FS_CHANNEL_IDENTIFICATION;
		i2s->sai_tx.FrameInit.FSPolarity = SAI_FS_ACTIVE_LOW;
		i2s->sai_tx.FrameInit.FSOffset = SAI_FS_BEFOREFIRSTBIT;

		i2s->sai_tx.SlotInit.FirstBitOffset = 0;
		i2s->sai_tx.SlotInit.SlotSize = SAI_SLOTSIZE_DATASIZE;
		i2s->sai_tx.SlotInit.SlotNumber = slot_count;
		i2s->sai_tx.SlotInit.SlotActive = slot_active;

		if (HAL_SAI_Init(&i2s->sai_tx) != HAL_OK)
			return OVE_ERR_NOT_SUPPORTED;
	}

	/* Configure RX SAI (slave, synchronous to TX) */
	if (cfg->direction & OVE_I2S_DIR_RX) {
		memset(&i2s->sai_rx, 0, sizeof(i2s->sai_rx));
		i2s->sai_rx.Instance = sai_rx_block;
		i2s->sai_rx.Init.AudioFrequency = cfg->sample_rate;
		i2s->sai_rx.Init.AudioMode = SAI_MODESLAVE_RX;
		i2s->sai_rx.Init.NoDivider = SAI_MASTERDIVIDER_ENABLED;
		i2s->sai_rx.Init.Protocol = SAI_FREE_PROTOCOL;
		i2s->sai_rx.Init.DataSize = data_size;
		i2s->sai_rx.Init.FirstBit = SAI_FIRSTBIT_MSB;
		i2s->sai_rx.Init.ClockStrobing = SAI_CLOCKSTROBING_RISINGEDGE;
		i2s->sai_rx.Init.Synchro = SAI_SYNCHRONOUS;
		i2s->sai_rx.Init.OutputDrive = SAI_OUTPUTDRIVE_DISABLED;
		i2s->sai_rx.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_FULL;
		/* MonoStereoMode left at default SAI_STEREOMODE — see TX-side
		 * comment above; collapsing to mono here would cause the audio
		 * source's stride-2 read to skip every other sample. */

		i2s->sai_rx.FrameInit.FrameLength = frame_length;
		i2s->sai_rx.FrameInit.ActiveFrameLength = active_frame_length;
		i2s->sai_rx.FrameInit.FSDefinition = SAI_FS_CHANNEL_IDENTIFICATION;
		i2s->sai_rx.FrameInit.FSPolarity = SAI_FS_ACTIVE_LOW;
		i2s->sai_rx.FrameInit.FSOffset = SAI_FS_BEFOREFIRSTBIT;

		i2s->sai_rx.SlotInit.FirstBitOffset = 0;
		i2s->sai_rx.SlotInit.SlotSize = SAI_SLOTSIZE_DATASIZE;
		i2s->sai_rx.SlotInit.SlotNumber = slot_count;
		i2s->sai_rx.SlotInit.SlotActive = slot_active;

		if (HAL_SAI_Init(&i2s->sai_rx) != HAL_OK)
			return OVE_ERR_NOT_SUPPORTED;
	}

	/* Enable SAI blocks immediately so MCLK/SCK/FS clocks are
	 * running before codec init.  The codec needs reference clocks
	 * during its I2C register configuration. */
	if (cfg->direction & OVE_I2S_DIR_TX)
		__HAL_SAI_ENABLE(&i2s->sai_tx);
	if (cfg->direction & OVE_I2S_DIR_RX)
		__HAL_SAI_ENABLE(&i2s->sai_rx);

	return OVE_OK;
}

void ove_hal_i2s_close(ove_i2s_t i2s)
{
	if (i2s->direction & OVE_I2S_DIR_TX)
		HAL_SAI_DeInit(&i2s->sai_tx);
	if (i2s->direction & OVE_I2S_DIR_RX)
		HAL_SAI_DeInit(&i2s->sai_rx);
	g_i2s_instance = NULL;
}

int ove_hal_i2s_start(ove_i2s_t i2s)
{
	size_t total_samples = i2s->dma_buf_samples;

	/* Pre-fill buffers with silence */
	if (i2s->tx_dma_buf)
		memset(i2s->tx_dma_buf, 0, total_samples * (i2s->bit_depth / 8));
	if (i2s->rx_dma_buf)
		memset(i2s->rx_dma_buf, 0, total_samples * (i2s->bit_depth / 8));

	/* TX starts first — master generates clocks for slave RX */
	if (i2s->direction & OVE_I2S_DIR_TX) {
		if (HAL_SAI_Transmit_DMA(&i2s->sai_tx, i2s->tx_dma_buf, (uint16_t)total_samples) !=
		    HAL_OK)
			return OVE_ERR_NOT_SUPPORTED;
	}

	if (i2s->direction & OVE_I2S_DIR_RX) {
		if (HAL_SAI_Receive_DMA(&i2s->sai_rx, i2s->rx_dma_buf, (uint16_t)total_samples) !=
		    HAL_OK)
			return OVE_ERR_NOT_SUPPORTED;
	}

	return OVE_OK;
}

int ove_hal_i2s_stop(ove_i2s_t i2s)
{
	if (i2s->direction & OVE_I2S_DIR_RX)
		HAL_SAI_DMAStop(&i2s->sai_rx);
	if (i2s->direction & OVE_I2S_DIR_TX)
		HAL_SAI_DMAStop(&i2s->sai_tx);
	return OVE_OK;
}

int ove_hal_i2s_pause(ove_i2s_t i2s)
{
	if (i2s->direction & OVE_I2S_DIR_RX)
		HAL_SAI_DMAPause(&i2s->sai_rx);
	if (i2s->direction & OVE_I2S_DIR_TX)
		HAL_SAI_DMAPause(&i2s->sai_tx);
	return OVE_OK;
}

int ove_hal_i2s_resume(ove_i2s_t i2s)
{
	if (i2s->direction & OVE_I2S_DIR_TX)
		HAL_SAI_DMAResume(&i2s->sai_tx);
	if (i2s->direction & OVE_I2S_DIR_RX)
		HAL_SAI_DMAResume(&i2s->sai_rx);
	return OVE_OK;
}

/* ── HAL SAI callbacks → portable ISR dispatch ───────────────── */

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
	(void)hsai;
	if (g_i2s_instance)
		ove_i2s_rx_half_cplt_isr(g_i2s_instance);
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
	(void)hsai;
	if (g_i2s_instance)
		ove_i2s_rx_cplt_isr(g_i2s_instance);
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
	(void)hsai;
	if (g_i2s_instance)
		ove_i2s_tx_half_cplt_isr(g_i2s_instance);
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
	(void)hsai;
	if (g_i2s_instance)
		ove_i2s_tx_cplt_isr(g_i2s_instance);
}

#endif /* CONFIG_OVE_I2S */
