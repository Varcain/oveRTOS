/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file i2c.hpp
 * @brief I2C bus master with RAII lifecycle
 */

#pragma once

#ifdef CONFIG_OVE_I2C

#include <ove/i2c.h>
#include <ove/types.hpp>

namespace ove
{

/**
 * @class I2c
 * @brief RAII wrapper around an oveRTOS I2C bus controller.
 *
 * Not copyable.  Move-only when heap allocation is enabled.
 */
class I2c
{
      public:
	/**
	 * @brief Construct and initialise the I2C bus from `cfg`.
	 * @param[in] cfg Bus configuration (pins, clock, mode).
	 */
	explicit I2c(const struct ove_i2c_cfg &cfg)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_i2c_init(&handle_, &storage_, &cfg);
#else
		int err = ove_i2c_create(&handle_, &cfg);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	~I2c()
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_i2c_deinit(handle_);
#else
		ove_i2c_destroy(handle_);
#endif
	}

	I2c(const I2c &) = delete;
	I2c &operator=(const I2c &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	I2c(I2c &&) = delete;
	I2c &operator=(I2c &&) = delete;
#else
	/** @brief Move constructor — transfers handle; source becomes empty. */
	I2c(I2c &&o) noexcept : handle_(o.handle_)
	{
		o.handle_ = nullptr;
	}
	/** @brief Move-assignment — destroys current bus, then takes `o`'s handle. */
	I2c &operator=(I2c &&o) noexcept
	{
		if (this != &o) {
			if (handle_)
				ove_i2c_destroy(handle_);
			handle_ = o.handle_;
			o.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/** @brief Write `len` bytes to slave `addr`. */
	[[nodiscard]] int write(uint16_t addr, const void *data, size_t len,
				std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_i2c_write(handle_, addr, data, len, to_timeout_ns(timeout));
	}

	/** @brief Read `len` bytes from slave `addr` into `buf`. */
	[[nodiscard]] int read(uint16_t addr, void *buf, size_t len,
			       std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_i2c_read(handle_, addr, buf, len, to_timeout_ns(timeout));
	}

	/** @brief Combined write-then-read transaction with a repeated start. */
	[[nodiscard]] int write_read(uint16_t addr, const void *tx, size_t tx_len, void *rx,
				     size_t rx_len, std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_i2c_write_read(handle_, addr, tx, tx_len, rx, rx_len, to_timeout_ns(timeout));
	}

	/** @brief Write `len` bytes to register `reg` on slave `addr`. */
	[[nodiscard]] int reg_write(uint16_t addr, uint8_t reg, const void *data, size_t len,
				    std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_i2c_reg_write(handle_, addr, reg, data, len, to_timeout_ns(timeout));
	}

	/** @brief Read `len` bytes from register `reg` on slave `addr`. */
	[[nodiscard]] int reg_read(uint16_t addr, uint8_t reg, void *buf, size_t len,
				   std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_i2c_reg_read(handle_, addr, reg, buf, len, to_timeout_ns(timeout));
	}

	/** @brief Probe slave `addr` — returns `OVE_OK` if the device ACKs. */
	[[nodiscard]] int probe(uint16_t addr, std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_i2c_probe(handle_, addr, to_timeout_ns(timeout));
	}

	/** @brief Returns the underlying C handle. */
	ove_i2c_t handle() const
	{
		return handle_;
	}

      private:
	ove_i2c_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_i2c_storage_t storage_{};
#endif
};

} /* namespace ove */

#endif /* CONFIG_OVE_I2C */
