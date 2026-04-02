/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_I2C_H
#define OVE_I2C_H

/**
 * @defgroup ove_i2c I2C
 * @brief I2C bus master driver.
 *
 * Provides a portable I2C master API with configurable bus speed,
 * thread-safe bus locking, register-level convenience functions,
 * and device probing.
 *
 * Two allocation strategies are supported:
 * - @c _create() / @c _destroy() — unified API that works in both heap and
 *   zero-heap mode.  In zero-heap mode these are macros that generate
 *   per-call-site static storage.
 * - @c _init() / @c _deinit() — explicit storage control with caller-supplied
 *   buffers.  Use when creating objects in loops, arrays, or structs.
 *
 * All addresses are 7-bit (e.g. 0x50 for a typical EEPROM).  The HAL
 * shifts left and adds the R/W bit internally.
 *
 * @note Requires @c CONFIG_OVE_I2C.  When the option is disabled every
 *       function is replaced by a no-op stub that returns
 *       @c OVE_ERR_NOT_SUPPORTED.
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
 * @brief I2C bus speed grade.
 */
typedef enum {
	OVE_I2C_SPEED_STANDARD  = 0, /**< 100 kHz. */
	OVE_I2C_SPEED_FAST      = 1, /**< 400 kHz. */
	OVE_I2C_SPEED_FAST_PLUS = 2, /**< 1 MHz. */
} ove_i2c_speed_t;

/* ── Configuration ───────────────────────────────────────────────── */

/**
 * @brief I2C bus configuration descriptor.
 */
struct ove_i2c_cfg {
	unsigned int    instance; /**< Peripheral index (0, 1, 2 ...). */
	ove_i2c_speed_t speed;    /**< Bus speed grade. */
};

#ifdef CONFIG_OVE_I2C

/* ── Lifecycle ───────────────────────────────────────────────────── */

/**
 * @brief Initialise an I2C bus using caller-provided static storage.
 *
 * @param[out] i2c     Receives the initialised I2C handle.
 * @param[in]  storage Pointer to statically-allocated I2C storage.
 * @param[in]  cfg     Bus configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_i2c_init(ove_i2c_t *i2c, ove_i2c_storage_t *storage,
		  const struct ove_i2c_cfg *cfg);

/**
 * @brief Deinitialise a statically-allocated I2C bus.
 *
 * @param[in] i2c  I2C handle returned by @ref ove_i2c_init.
 */
void ove_i2c_deinit(ove_i2c_t i2c);

/**
 * @brief Create a heap-allocated I2C bus controller.
 *
 * @param[out] i2c  Receives the created I2C handle.
 * @param[in]  cfg  Bus configuration descriptor.
 * @return OVE_OK on success, negative error code on failure.
 */
#ifdef OVE_HEAP_I2C
int  ove_i2c_create(ove_i2c_t *i2c, const struct ove_i2c_cfg *cfg);

/**
 * @brief Destroy a heap-allocated I2C bus controller.
 *
 * @param[in] i2c  I2C handle returned by @ref ove_i2c_create.
 */
void ove_i2c_destroy(ove_i2c_t i2c);
#elif !defined(__ZIG_CIMPORT__)
#define ove_i2c_create(pi2c, cfg) \
	({ static ove_i2c_storage_t _ove_stor_; \
	   ove_i2c_init((pi2c), &_ove_stor_, (cfg)); })
#define ove_i2c_destroy(i2c) ove_i2c_deinit(i2c)
#endif

/* ── Operations ──────────────────────────────────────────────────── */

/**
 * @brief Write data to an I2C device.
 *
 * @param[in] i2c        I2C handle.
 * @param[in] addr       7-bit device address.
 * @param[in] data       Data to write.
 * @param[in] len        Number of bytes to write.
 * @param[in] timeout_ms Maximum wait time; @c OVE_WAIT_FOREVER to block.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_i2c_write(ove_i2c_t i2c, uint16_t addr,
		   const void *data, size_t len, uint32_t timeout_ms);

/**
 * @brief Read data from an I2C device.
 *
 * @param[in]  i2c        I2C handle.
 * @param[in]  addr       7-bit device address.
 * @param[out] buf        Buffer to receive data.
 * @param[in]  len        Number of bytes to read.
 * @param[in]  timeout_ms Maximum wait time; @c OVE_WAIT_FOREVER to block.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_i2c_read(ove_i2c_t i2c, uint16_t addr,
		  void *buf, size_t len, uint32_t timeout_ms);

/**
 * @brief Combined write-then-read with I2C repeated start.
 *
 * Writes @p tx_len bytes from @p tx, then reads @p rx_len bytes into
 * @p rx without releasing the bus (repeated start condition).  This is
 * the standard pattern for register reads on I2C sensors and codecs.
 *
 * @param[in]  i2c        I2C handle.
 * @param[in]  addr       7-bit device address.
 * @param[in]  tx         Transmit buffer (e.g. register address).
 * @param[in]  tx_len     Number of bytes to write.
 * @param[out] rx         Receive buffer.
 * @param[in]  rx_len     Number of bytes to read.
 * @param[in]  timeout_ms Maximum wait time; @c OVE_WAIT_FOREVER to block.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_i2c_write_read(ove_i2c_t i2c, uint16_t addr,
			const void *tx, size_t tx_len,
			void *rx, size_t rx_len,
			uint32_t timeout_ms);

/* ── Register convenience ────────────────────────────────────────── */

/**
 * @brief Write to a single-byte-addressed register.
 *
 * Prepends @p reg to @p data and performs a single I2C write.
 * Implemented in the portable layer — not a HAL function.
 *
 * @param[in] i2c        I2C handle.
 * @param[in] addr       7-bit device address.
 * @param[in] reg        Register address byte.
 * @param[in] data       Data to write after the register byte.
 * @param[in] len        Number of data bytes.
 * @param[in] timeout_ms Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_i2c_reg_write(ove_i2c_t i2c, uint16_t addr, uint8_t reg,
		       const void *data, size_t len,
		       uint32_t timeout_ms);

/**
 * @brief Read from a single-byte-addressed register.
 *
 * Writes @p reg, then reads @p len bytes via repeated start.
 * Implemented in the portable layer — not a HAL function.
 *
 * @param[in]  i2c        I2C handle.
 * @param[in]  addr       7-bit device address.
 * @param[in]  reg        Register address byte.
 * @param[out] buf        Buffer to receive register data.
 * @param[in]  len        Number of bytes to read.
 * @param[in]  timeout_ms Maximum wait time.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_i2c_reg_read(ove_i2c_t i2c, uint16_t addr, uint8_t reg,
		      void *buf, size_t len, uint32_t timeout_ms);

/* ── Bus probe ───────────────────────────────────────────────────── */

/**
 * @brief Probe for a device at the given address.
 *
 * Sends a zero-length write and checks for ACK.  Useful for device
 * enumeration and presence detection.
 *
 * @param[in] i2c        I2C handle.
 * @param[in] addr       7-bit device address to probe.
 * @param[in] timeout_ms Maximum wait time.
 * @return OVE_OK if the device ACKs, OVE_ERR_BUS_NACK if not,
 *         other negative error code on bus failure.
 */
int  ove_i2c_probe(ove_i2c_t i2c, uint16_t addr, uint32_t timeout_ms);

#else /* !CONFIG_OVE_I2C */

static inline int  ove_i2c_create(ove_i2c_t *i, const struct ove_i2c_cfg *c) { (void)i; (void)c; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_i2c_destroy(ove_i2c_t i) { (void)i; }
static inline int  ove_i2c_write(ove_i2c_t i, uint16_t a, const void *d, size_t l, uint32_t t) { (void)i; (void)a; (void)d; (void)l; (void)t; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_i2c_read(ove_i2c_t i, uint16_t a, void *b, size_t l, uint32_t t) { (void)i; (void)a; (void)b; (void)l; (void)t; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_i2c_write_read(ove_i2c_t i, uint16_t a, const void *tx, size_t tl, void *rx, size_t rl, uint32_t t) { (void)i; (void)a; (void)tx; (void)tl; (void)rx; (void)rl; (void)t; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_i2c_reg_write(ove_i2c_t i, uint16_t a, uint8_t r, const void *d, size_t l, uint32_t t) { (void)i; (void)a; (void)r; (void)d; (void)l; (void)t; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_i2c_reg_read(ove_i2c_t i, uint16_t a, uint8_t r, void *b, size_t l, uint32_t t) { (void)i; (void)a; (void)r; (void)b; (void)l; (void)t; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_i2c_probe(ove_i2c_t i, uint16_t a, uint32_t t) { (void)i; (void)a; (void)t; return OVE_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_OVE_I2C */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_I2C_H */
