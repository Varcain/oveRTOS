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
 * Timeouts: only the FreeRTOS STM32 driver honours @c timeout_ns, rounded
 * down to whole milliseconds (submillisecond values give a zero HAL wait budget).
 * Zephyr and NuttX ignore it and rely on their drivers' own timeouts.  On
 * every engine the call first waits, with no timeout, for the bus lock held
 * by another synchronous transfer through the same ove handle.
 *
 * Transfer counts are bytes in both 8-bit and 16-bit modes. In 16-bit mode
 * every nonempty count must be even and every non-NULL buffer must be aligned
 * for uint16_t. Each buffer must cover the entire byte count. TX/RX may be
 * disjoint or exactly identical for in-place operation; partial overlap is
 * rejected. Frame, pointer-presence, alignment, overlap and backend limits are
 * checked before software CS or transfer state changes; all nonempty sequence
 * records are checked first. The caller supplies the full valid memory spans.
 *
 * Locking excludes other synchronous calls through the same ove handle.
 * Coordinate synchronous and asynchronous use: async does not take that mutex.
 * A sequence retains its supplied software CS across segments; native hardware CS and native-global
 * controller locking may end between segments (notably on NuttX and Zephyr).
 * Independently initialized handles and native driver users need external
 * coordination. Keep the handle's configuration stable during each call.
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
OVE_NODISCARD int ove_spi_init(ove_spi_t *spi, ove_spi_storage_t *storage,
			       const struct ove_spi_cfg *cfg);
/** @brief Release an SPI bus handle previously created with `ove_spi_init`.
 * Thread context only. Consumes the handle: coordinate existing/new callers
 * externally. An accepted async operation defers cleanup until native retirement;
 * missing completion retains the backing. Keep storage and callback userdata
 * alive through BOTH this call returning and final callback completion. A late
 * concurrent close may overlap callback dispatch; callback alone does not prove
 * an independently active close call has returned. Do not reuse the consumed
 * handle from its callback. Synchronous callers must already be quiescent.
 */
void ove_spi_deinit(ove_spi_t spi);

#ifdef OVE_HEAP_SPI
/**
 * @brief Heap-mode counterpart of `ove_spi_init()` — allocates storage internally.
 * @param[out] spi Receives the initialised handle.
 * @param[in]  cfg Bus configuration.
 * @return OVE_OK on success, negative error code on failure.
 */
OVE_NODISCARD int ove_spi_create(ove_spi_t *spi, const struct ove_spi_cfg *cfg);
/** @brief Destroy a heap-created handle; same consumed-handle and pending
 * retirement obligations as ove_spi_deinit(). Native heap storage is freed
 * only after outstanding provider/submission accesses end. Callback userdata
 * must be separately valid through callback completion.
 */
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
OVE_NODISCARD int ove_spi_transfer(ove_spi_t spi, const struct ove_spi_cs *cs, const void *tx,
				   void *rx, size_t len, uint64_t timeout_ns);

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
OVE_NODISCARD int ove_spi_write(ove_spi_t spi, const struct ove_spi_cs *cs, const void *data,
				size_t len, uint64_t timeout_ns);

/**
 * @brief Read-only SPI transfer with backend-defined transmit filler.
 *
 * Filler is not necessarily zero: it may be all ones or the initial contents
 * of the RX buffer. Use an explicit TX buffer when transmitted values matter.
 *
 * @param[in]  spi        SPI handle.
 * @param[in]  cs         Chip-select descriptor, or NULL.
 * @param[out] buf        Buffer to receive data.
 * @param[in]  len        Number of bytes.
 * @param[in]  timeout_ns Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
OVE_NODISCARD int ove_spi_read(ove_spi_t spi, const struct ove_spi_cs *cs, void *buf, size_t len,
			       uint64_t timeout_ns);

/**
 * @brief Multi-segment SPI transfer under a single CS assertion.
 *
 * Executes @p num_xfers transfer segments sequentially without
 * releasing CS between them.  Useful for protocols that require
 * command + address + data in one transaction.
 * Zero-length records are skipped. The supplied software CS and same-handle
 * mutex span the sequence; native hardware selection/locking may not.
 *
 * @param[in] spi        SPI handle.
 * @param[in] cs         Chip-select descriptor, or NULL.
 * @param[in] xfers      Array of transfer segments.
 * @param[in] num_xfers  Number of segments.
 * @param[in] timeout_ns Maximum wait time for each segment, not the whole
 *                       sequence (see the timeout note on @ref ove_spi).
 * @return OVE_OK on success, negative error code on failure.
 */
OVE_NODISCARD int ove_spi_transfer_seq(ove_spi_t spi, const struct ove_spi_cs *cs,
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
 * Submit from thread context. Final notification runs in task context after
 * the submission and provider's last controller access. FreeRTOS uses its timer
 * daemon after IT completion; Zephyr uses its system workqueue after the
 * embedded transfer work's kernel tail; NuttX/host use the final worker or
 * submitter access. A callback can precede this function's return, but neither
 * the native submission nor provider touches retained backing afterward.
 * Callbacks must be non-blocking. STM32 callbacks previously assuming ISR
 * context must migrate unconditional FromISR-only calls to task APIs.
 * These SPI rules do not change I2C or other DMA callback contexts.
 *
 * Buffers, CS and userdata remain valid through final callback completion and
 * every active API call returning. Callback notification alone does not
 * authorize releasing backing while a concurrent close call is still active.
 * A callback may submit another operation on a live, unconsumed handle.
 * The backend must signal exactly one result for accepted work and none for
 * rejection. A never-completing operation retains one live controller.
 *
 * FreeRTOS and Zephyr lazily start one static 10ms retry timer (rounded up to
 * at least one tick). Coalesced daemon notifications are the FreeRTOS fast path; queue-full
 * leaves retained nodes for the periodic pump. Each visit drains a bounded
 * batch. This adds an idle O(1) check per period and no task stack reservation;
 * daemon/worker starvation has no hard retirement deadline. Zephyr retains
 * rejected notifications while its system queue is draining/plugged and retries
 * once that queue becomes available. Preparation can fail
 * before publication if that service cannot be established.
 * NuttX workers allocate a native TCB/stack per submission. Release zero-heap
 * builds use the configured reserved heap and can return allocation failure;
 * zero-heap builds with CONFIG_DEBUG_ASSERTIONS are unsupported because their
 * post-lock allocation policy asserts before reserved allocation. Preparation
 * returns OVE_ERR_NOT_SUPPORTED before publishing work in that configuration.
 *
 * Whole-frame/alignment/overlap rules also apply here. The STM32 FreeRTOS
 * adapter accepts at most 65535 native frames in one async submission (65535
 * bytes in 8-bit mode, 131070 in 16-bit mode); a larger request is rejected
 * before CS or pending-state publication. Its synchronous path splits larger
 * requests into whole-frame chunks under one per-segment timeout budget.
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
OVE_NODISCARD int ove_spi_transfer_async(ove_spi_t spi, const struct ove_spi_cs *cs, const void *tx,
					 void *rx, size_t len, ove_dma_complete_cb cb,
					 void *user_data);

/** Prepare async completion dispatch from thread context, without publishing
 * an operation. Optional for C callers (submission prepares automatically);
 * ownership bindings may call this before constructing an owner. Requires
 * CONFIG_OVE_ASYNC. Returns OVE_OK or a negative setup error.
 */
OVE_NODISCARD int ove_spi_async_prepare(void);

#ifdef CONFIG_OVE_ASYNC
/** Validate an owned binding's nonempty byte count before staging allocation.
 * Internal binding support. Requires a live, configured handle and external
 * lifetime/configuration coordination. Checks whole frames and the existing
 * provider limit without admitting work, touching CS or retaining pointers.
 */
OVE_NODISCARD int ove_spi_binding_check_length(ove_spi_t spi, size_t len);

#ifdef OVE_HEAP_SPI
/** Allocate stable SPI binding backing with the requested power-of-two alignment.
 * Internal binding support using the configured native allocator. Returns NULL
 * on invalid/overflowing layout or allocation failure. Size must be nonzero;
 * the complete padded allocation must fit PTRDIFF_MAX. Padding includes one
 * saved allocation pointer and up to alignment-1 bytes. No Rust allocator or
 * global pool is involved. Free only with ove_spi_binding_free().
 */
OVE_NODISCARD void *ove_spi_binding_alloc(size_t size, size_t alignment);

/** Release only a live allocation returned by ove_spi_binding_alloc(), or NULL.
 * The caller must retire all users before freeing; this function does not
 * provide operation ownership, cancellation or reference counting.
 */
void ove_spi_binding_free(void *pointer);
#endif
#endif

#else /* !CONFIG_OVE_SPI */

OVE_NODISCARD static inline int ove_spi_async_prepare(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}

/* No _init/_deinit stubs: OVE_SPI_DEFINE_STATIC is itself gated by
 * #ifdef CONFIG_OVE_SPI in storage.h. */
OVE_NODISCARD static inline int ove_spi_create(ove_spi_t *s, const struct ove_spi_cfg *c)
{
	(void)s;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_spi_destroy(ove_spi_t s)
{
	(void)s;
}
OVE_NODISCARD static inline int ove_spi_transfer_async(ove_spi_t s, const struct ove_spi_cs *cs,
						       const void *tx, void *rx, size_t l,
						       ove_dma_complete_cb cb, void *ud)
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
OVE_NODISCARD static inline int ove_spi_transfer(ove_spi_t s, const struct ove_spi_cs *cs,
						 const void *tx, void *rx, size_t l, uint64_t t)
{
	(void)s;
	(void)cs;
	(void)tx;
	(void)rx;
	(void)l;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
OVE_NODISCARD static inline int ove_spi_write(ove_spi_t s, const struct ove_spi_cs *cs,
					      const void *d, size_t l, uint64_t t)
{
	(void)s;
	(void)cs;
	(void)d;
	(void)l;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
OVE_NODISCARD static inline int ove_spi_read(ove_spi_t s, const struct ove_spi_cs *cs, void *b,
					     size_t l, uint64_t t)
{
	(void)s;
	(void)cs;
	(void)b;
	(void)l;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
OVE_NODISCARD static inline int ove_spi_transfer_seq(ove_spi_t s, const struct ove_spi_cs *cs,
						     const struct ove_spi_xfer *x, unsigned int n,
						     uint64_t t)
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
