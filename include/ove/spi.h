/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_SPI_H
#define OVE_SPI_H

/**
 * @file spi.h
 * @defgroup ove_spi SPI
 * @brief SPI bus master driver.
 *
 * Provides a portable SPI master API with configurable clock, mode,
 * thread-safe bus locking, and software chip-select management via GPIO.
 *
 * Two allocation strategies are supported:
 * - @c _create() / @c _destroy() — heap-allocated.  Available only when
 *   @c OVE_HEAP_SPI is defined (i.e. @c CONFIG_OVE_ZERO_HEAP is not set).
 * - @c _init() / @c _deinit() — caller-supplied storage.  Available in both
 *   modes.  See @c OVE_SPI_DEFINE_STATIC for a one-step static helper.
 *
 * @note Requires @c CONFIG_OVE_SPI.
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
 * @brief SPI clock polarity / phase mode.
 */
typedef enum {
	OVE_SPI_MODE_0 = 0, /**< CPOL=0, CPHA=0. */
	OVE_SPI_MODE_1 = 1, /**< CPOL=0, CPHA=1. */
	OVE_SPI_MODE_2 = 2, /**< CPOL=1, CPHA=0. */
	OVE_SPI_MODE_3 = 3, /**< CPOL=1, CPHA=1. */
} ove_spi_mode_t;

/**
 * @brief SPI bit order.
 */
typedef enum {
	OVE_SPI_MSB_FIRST = 0, /**< Most significant bit first (common). */
	OVE_SPI_LSB_FIRST = 1, /**< Least significant bit first. */
} ove_spi_bit_order_t;

/* ── Configuration ───────────────────────────────────────────────── */

/**
 * @brief SPI bus configuration descriptor.
 */
struct ove_spi_cfg {
	unsigned int instance;	       /**< SPI peripheral index (0, 1, 2 ...). */
	uint32_t clock_hz;	       /**< SCK frequency in Hz. */
	ove_spi_mode_t mode;	       /**< Clock polarity / phase. */
	ove_spi_bit_order_t bit_order; /**< Bit order on the wire. */
	uint8_t word_size;	       /**< Bits per word: 8 or 16. */
};

/**
 * @brief SPI chip-select descriptor.
 *
 * Identifies the GPIO pin used for software CS management.
 * Pass @c NULL to SPI transfer functions to skip CS handling
 * (hardware CS or external management).
 */
struct ove_spi_cs {
	unsigned int gpio_port; /**< GPIO port index for CS pin. */
	unsigned int gpio_pin;	/**< GPIO pin index for CS pin. */
	int active_low;		/**< Non-zero if CS is active low (common). */
};

/**
 * @brief SPI transfer segment for multi-segment transactions.
 *
 * Used with ove_spi_transfer_seq() to perform multiple transfer
 * segments under a single CS assertion.
 */
struct ove_spi_xfer {
	const void *tx; /**< TX buffer, or NULL for RX-only. */
	void *rx;	/**< RX buffer, or NULL for TX-only. */
	size_t len;	/**< Number of bytes in this segment. */
};

#ifdef CONFIG_OVE_SPI

/* ── Lifecycle ───────────────────────────────────────────────────── */

/**
 * @brief Initialise an SPI bus controller with caller-provided storage.
 * @param[out] spi     Receives the initialised handle.
 * @param[in]  storage Statically-allocated SPI storage.
 * @param[in]  cfg     Bus configuration (pins, mode, clock rate).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_spi_init(ove_spi_t *spi, ove_spi_storage_t *storage, const struct ove_spi_cfg *cfg);
/** @brief Release an SPI bus handle previously created with `ove_spi_init`. */
void ove_spi_deinit(ove_spi_t spi);

#ifdef OVE_HEAP_SPI
/**
 * @brief Heap-mode counterpart of `ove_spi_init()` — allocates storage internally.
 * @param[out] spi Receives the initialised handle.
 * @param[in]  cfg Bus configuration.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_spi_create(ove_spi_t *spi, const struct ove_spi_cfg *cfg);
/** @brief Destroy an SPI bus handle previously created with `ove_spi_create`. */
void ove_spi_destroy(ove_spi_t spi);
#endif /* OVE_HEAP_SPI */

/* ── Operations ──────────────────────────────────────────────────── */

/**
 * @brief Full-duplex SPI transfer.
 *
 * Simultaneously transmits from @p tx and receives into @p rx.
 * Either @p tx or @p rx may be NULL for half-duplex operation.
 * CS is asserted before and deasserted after the transfer.
 *
 * @param[in]  spi        SPI handle.
 * @param[in]  cs         Chip-select descriptor, or NULL to skip CS.
 * @param[in]  tx         Transmit buffer, or NULL.
 * @param[out] rx         Receive buffer, or NULL.
 * @param[in]  len        Number of bytes to transfer.
 * @param[in]  timeout_ns Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_spi_transfer(ove_spi_t spi, const struct ove_spi_cs *cs, const void *tx, void *rx,
		     size_t len, uint64_t timeout_ns);

/**
 * @brief Write-only SPI transfer (TX only, ignore RX).
 *
 * @param[in] spi        SPI handle.
 * @param[in] cs         Chip-select descriptor, or NULL.
 * @param[in] data       Data to transmit.
 * @param[in] len        Number of bytes.
 * @param[in] timeout_ns Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_spi_write(ove_spi_t spi, const struct ove_spi_cs *cs, const void *data, size_t len,
		  uint64_t timeout_ns);

/**
 * @brief Read-only SPI transfer (clock out zeros, capture RX).
 *
 * @param[in]  spi        SPI handle.
 * @param[in]  cs         Chip-select descriptor, or NULL.
 * @param[out] buf        Buffer to receive data.
 * @param[in]  len        Number of bytes.
 * @param[in]  timeout_ns Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_spi_read(ove_spi_t spi, const struct ove_spi_cs *cs, void *buf, size_t len,
		 uint64_t timeout_ns);

/**
 * @brief Multi-segment SPI transfer under a single CS assertion.
 *
 * Executes @p num_xfers transfer segments sequentially without
 * releasing CS between them.  Useful for protocols that require
 * command + address + data in one transaction.
 *
 * @param[in] spi        SPI handle.
 * @param[in] cs         Chip-select descriptor, or NULL.
 * @param[in] xfers      Array of transfer segments.
 * @param[in] num_xfers  Number of segments.
 * @param[in] timeout_ns Maximum wait time for the entire sequence.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_spi_transfer_seq(ove_spi_t spi, const struct ove_spi_cs *cs,
			 const struct ove_spi_xfer *xfers, unsigned int num_xfers,
			 uint64_t timeout_ns);

/**
 * @brief Submit an SPI transfer asynchronously and return immediately.
 *
 * Initiates a full-duplex transfer (TX from @p tx, RX into @p rx; either
 * may be NULL but not both). The call returns @c OVE_OK as soon as the
 * transfer has been accepted by the backend — completion is signalled
 * via @p cb.
 *
 * Completion context is backend-dependent:
 *  - STM32F7 + FreeRTOS: DMA TC ISR (callable from interrupt context).
 *  - Zephyr: poll signal handler (typically a system workqueue thread).
 *  - NuttX / POSIX: a worker thread that runs the blocking transfer
 *    and invokes @p cb on return.  No real DMA — provides the async
 *    surface for testing.
 *
 * The callback must be non-blocking and ISR-safe.  Higher-level
 * bindings (Rust @c AsyncSpi) wrap this with @c AtomicWaker.
 *
 * @param[in] spi        SPI handle.
 * @param[in] cs         Chip-select descriptor, or NULL.
 * @param[in] tx         Transmit buffer, or NULL.  Must outlive the
 *                       transfer.
 * @param[in] rx         Receive buffer, or NULL.  Must outlive the
 *                       transfer.
 * @param[in] len        Number of bytes to transfer.
 * @param[in] cb         Completion callback.  Must not be NULL.
 * @param[in] user_data  Opaque pointer forwarded to @p cb.
 * @return OVE_OK if accepted (transfer in flight; callback will fire),
 *         negative error code if the submission itself failed (the
 *         callback will NOT fire in this case).
 */
int ove_spi_transfer_async(ove_spi_t spi, const struct ove_spi_cs *cs, const void *tx, void *rx,
			   size_t len, ove_dma_complete_cb cb, void *user_data);

#else /* !CONFIG_OVE_SPI */

/* No _init/_deinit stubs: OVE_SPI_DEFINE_STATIC is itself gated by
 * #ifdef CONFIG_OVE_SPI in storage.h. */
static inline int ove_spi_create(ove_spi_t *s, const struct ove_spi_cfg *c)
{
	(void)s;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_spi_destroy(ove_spi_t s)
{
	(void)s;
}
static inline int ove_spi_transfer_async(ove_spi_t s, const struct ove_spi_cs *cs, const void *tx,
					 void *rx, size_t l, ove_dma_complete_cb cb, void *ud)
{
	(void)s;
	(void)cs;
	(void)tx;
	(void)rx;
	(void)l;
	(void)cb;
	(void)ud;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_spi_transfer(ove_spi_t s, const struct ove_spi_cs *cs, const void *tx,
				   void *rx, size_t l, uint64_t t)
{
	(void)s;
	(void)cs;
	(void)tx;
	(void)rx;
	(void)l;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_spi_write(ove_spi_t s, const struct ove_spi_cs *cs, const void *d, size_t l,
				uint64_t t)
{
	(void)s;
	(void)cs;
	(void)d;
	(void)l;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_spi_read(ove_spi_t s, const struct ove_spi_cs *cs, void *b, size_t l,
			       uint64_t t)
{
	(void)s;
	(void)cs;
	(void)b;
	(void)l;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_spi_transfer_seq(ove_spi_t s, const struct ove_spi_cs *cs,
				       const struct ove_spi_xfer *x, unsigned int n, uint64_t t)
{
	(void)s;
	(void)cs;
	(void)x;
	(void)n;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_SPI */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SPI_H */
