/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_I2S_H
#define OVE_I2S_H

/**
 * @defgroup ove_i2s I2S
 * @brief I2S / SAI audio bus driver.
 *
 * Provides a portable I2S master driver for DMA-based audio streaming
 * with double-buffered (ping-pong) operation.  The driver delivers
 * half-buffer completion callbacks from ISR context, enabling
 * real-time audio processing.
 *
 * The codec connected to the I2S bus is NOT initialised by this driver;
 * codec setup is board-specific and must be done separately (e.g. via
 * I2C register writes in the board BSP).
 *
 * @note Requires @c CONFIG_OVE_I2S.
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"
#include "ove/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Enums ───────────────────────────────────────────────────────── */

/**
 * @brief I2S stream direction.
 */
typedef enum {
	OVE_I2S_DIR_TX = 0x01,	 /**< Transmit only (playback). */
	OVE_I2S_DIR_RX = 0x02,	 /**< Receive only (capture). */
	OVE_I2S_DIR_TXRX = 0x03, /**< Full-duplex (simultaneous TX + RX). */
} ove_i2s_dir_t;

/* ── Callback ────────────────────────────────────────────────────── */

/**
 * @brief I2S half-buffer completion callback.
 *
 * Called from ISR context when a DMA half-transfer or full-transfer
 * completes.  The callback should be short — typically it unblocks a
 * processing task.
 *
 * @param[in] i2s       I2S handle.
 * @param[in] user_data Opaque pointer supplied at registration time.
 */
typedef void (*ove_i2s_cb_t)(ove_i2s_t i2s, void *user_data);

/* ── Configuration ───────────────────────────────────────────────── */

/**
 * @brief I2S bus configuration descriptor.
 */
struct ove_i2s_cfg {
	unsigned int instance;	 /**< I2S / SAI peripheral index (0, 1 ...). */
	uint32_t sample_rate;	 /**< Sample rate in Hz (e.g. 44100, 48000). */
	uint8_t bit_depth;	 /**< Bits per sample: 16, 24, or 32. */
	uint8_t channels;	 /**< Channel count: 1 (mono) or 2 (stereo). */
	ove_i2s_dir_t direction; /**< Stream direction. */
	size_t dma_buf_samples;	 /**< Total samples in DMA buffer (both halves). */
};

#ifdef CONFIG_OVE_I2S

/* ── Lifecycle ───────────────────────────────────────────────────── */

/**
 * @brief Initialise I2S with caller-provided static storage and DMA buffers.
 *
 * @param[out] i2s        Receives the initialised handle.
 * @param[in]  storage    Statically-allocated I2S storage.
 * @param[in]  tx_dma_buf TX DMA buffer (may be NULL if direction is RX-only).
 *                        Must be in DMA-accessible, cache-coherent memory.
 * @param[in]  rx_dma_buf RX DMA buffer (may be NULL if direction is TX-only).
 * @param[in]  cfg        I2S configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_i2s_init(ove_i2s_t *i2s, ove_i2s_storage_t *storage, void *tx_dma_buf, void *rx_dma_buf,
		 const struct ove_i2s_cfg *cfg);
/** @brief Release an I2S handle previously created with `ove_i2s_init`. */
void ove_i2s_deinit(ove_i2s_t i2s);

#ifdef OVE_HEAP_I2S
/**
 * @brief Heap-mode counterpart of `ove_i2s_init()` — allocates storage and
 *        DMA buffers internally.
 * @param[out] i2s Receives the initialised handle.
 * @param[in]  cfg I2S configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_i2s_create(ove_i2s_t *i2s, const struct ove_i2s_cfg *cfg);
/** @brief Destroy an I2S handle previously created with `ove_i2s_create`. */
void ove_i2s_destroy(ove_i2s_t i2s);
#endif /* OVE_HEAP_I2S */

/* ── Callbacks ───────────────────────────────────────────────────── */

/**
 * @brief Set the RX half-buffer completion callback.
 *
 * Called from ISR when a DMA RX half-buffer is ready for processing.
 */
int ove_i2s_set_rx_callback(ove_i2s_t i2s, ove_i2s_cb_t cb, void *user_data);

/**
 * @brief Set the TX half-buffer completion callback.
 *
 * Called from ISR when a DMA TX half-buffer has been transmitted and
 * is safe to refill.
 */
int ove_i2s_set_tx_callback(ove_i2s_t i2s, ove_i2s_cb_t cb, void *user_data);

/* ── Stream control ──────────────────────────────────────────────── */

/**
 * @brief Start I2S DMA streaming.
 *
 * Begins circular DMA transfers.  TX starts first to generate clocks
 * for a synchronous RX slave.
 */
int ove_i2s_start(ove_i2s_t i2s);

/**
 * @brief Stop I2S DMA streaming.
 */
int ove_i2s_stop(ove_i2s_t i2s);

/**
 * @brief Pause I2S DMA streaming (can be resumed).
 */
int ove_i2s_pause(ove_i2s_t i2s);

/**
 * @brief Resume I2S DMA streaming after pause.
 */
int ove_i2s_resume(ove_i2s_t i2s);

/* ── Buffer access ───────────────────────────────────────────────── */

/**
 * @brief Get pointer to the most recently completed RX half-buffer.
 *
 * Returns the half of the DMA RX buffer that was just filled and is
 * safe to read.  Call this from within the RX callback.
 *
 * @param[in] i2s  I2S handle.
 * @return Pointer to the completed RX half-buffer, or NULL on error.
 */
void *ove_i2s_rx_buf(ove_i2s_t i2s);

/**
 * @brief Get pointer to the TX half-buffer safe to write.
 *
 * Returns the half of the DMA TX buffer that DMA is NOT currently
 * transmitting from.  Fill this buffer before the next TX callback.
 *
 * @param[in] i2s  I2S handle.
 * @return Pointer to the writable TX half-buffer, or NULL on error.
 */
void *ove_i2s_tx_buf(ove_i2s_t i2s);

/**
 * @brief Get the size of one half-buffer in bytes.
 *
 * @param[in] i2s  I2S handle.
 * @return Half-buffer size in bytes.
 */
size_t ove_i2s_half_buf_size(ove_i2s_t i2s);

/* ── ISR helpers (called by backend, not by application) ─────────── */

/** @brief ISR helper — invoke from backend RX half-complete interrupt. */
void ove_i2s_rx_half_cplt_isr(ove_i2s_t i2s);
/** @brief ISR helper — invoke from backend RX full-complete interrupt. */
void ove_i2s_rx_cplt_isr(ove_i2s_t i2s);
/** @brief ISR helper — invoke from backend TX half-complete interrupt. */
void ove_i2s_tx_half_cplt_isr(ove_i2s_t i2s);
/** @brief ISR helper — invoke from backend TX full-complete interrupt. */
void ove_i2s_tx_cplt_isr(ove_i2s_t i2s);

#else /* !CONFIG_OVE_I2S */

/* No _init/_deinit stubs: OVE_I2S_DEFINE_STATIC is itself gated by
 * #ifdef CONFIG_OVE_I2S in storage.h. */
static inline int ove_i2s_create(ove_i2s_t *i, const struct ove_i2s_cfg *c)
{
	(void)i;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_i2s_destroy(ove_i2s_t i)
{
	(void)i;
}
static inline int ove_i2s_set_rx_callback(ove_i2s_t i, ove_i2s_cb_t cb, void *ud)
{
	(void)i;
	(void)cb;
	(void)ud;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_i2s_set_tx_callback(ove_i2s_t i, ove_i2s_cb_t cb, void *ud)
{
	(void)i;
	(void)cb;
	(void)ud;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_i2s_start(ove_i2s_t i)
{
	(void)i;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_i2s_stop(ove_i2s_t i)
{
	(void)i;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_i2s_pause(ove_i2s_t i)
{
	(void)i;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_i2s_resume(ove_i2s_t i)
{
	(void)i;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void *ove_i2s_rx_buf(ove_i2s_t i)
{
	(void)i;
	return (void *)0;
}
static inline void *ove_i2s_tx_buf(ove_i2s_t i)
{
	(void)i;
	return (void *)0;
}
static inline size_t ove_i2s_half_buf_size(ove_i2s_t i)
{
	(void)i;
	return 0;
}

#endif /* CONFIG_OVE_I2S */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_I2S_H */
