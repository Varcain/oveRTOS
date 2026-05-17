/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file spi.hpp
 * @brief SPI bus master with RAII lifecycle
 */

#pragma once

#ifdef CONFIG_OVE_SPI

#include <ove/spi.h>
#include <ove/types.hpp>

namespace ove
{

/**
 * @class Spi
 * @brief RAII wrapper around an oveRTOS SPI bus controller.
 *
 * Not copyable.  Move-only when heap allocation is enabled.
 */
class Spi
{
      public:
	/**
	 * @brief Construct and initialise the SPI bus from `cfg`.
	 * @param[in] cfg Bus configuration (pins, mode, clock rate).
	 */
	explicit Spi(const struct ove_spi_cfg &cfg)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_spi_init(&handle_, &storage_, &cfg);
#else
		int err = ove_spi_create(&handle_, &cfg);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	~Spi()
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_spi_deinit(handle_);
#else
		ove_spi_destroy(handle_);
#endif
	}

	Spi(const Spi &) = delete;
	Spi &operator=(const Spi &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Spi(Spi &&) = delete;
	Spi &operator=(Spi &&) = delete;
#else
	/** @brief Move constructor — transfers handle; source becomes empty. */
	Spi(Spi &&o) noexcept : handle_(o.handle_)
	{
		o.handle_ = nullptr;
	}
	/** @brief Move-assignment — destroys current bus, then takes `o`'s handle. */
	Spi &operator=(Spi &&o) noexcept
	{
		if (this != &o) {
			if (handle_)
				ove_spi_destroy(handle_);
			handle_ = o.handle_;
			o.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/** @brief Full-duplex transfer — sends `tx` and receives into `rx`. */
	[[nodiscard]] int transfer(const struct ove_spi_cs *cs, const void *tx, void *rx,
				   size_t len, std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_spi_transfer(handle_, cs, tx, rx, len, to_timeout_ns(timeout));
	}

	/** @brief Write-only transfer — receive data is discarded. */
	[[nodiscard]] int write(const struct ove_spi_cs *cs, const void *data, size_t len,
				std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_spi_write(handle_, cs, data, len, to_timeout_ns(timeout));
	}

	/** @brief Read-only transfer — transmit sends zeros. */
	[[nodiscard]] int read(const struct ove_spi_cs *cs, void *buf, size_t len,
			       std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_spi_read(handle_, cs, buf, len, to_timeout_ns(timeout));
	}

	/** @brief Execute a sequence of transfers under a single CS assertion. */
	[[nodiscard]] int transfer_seq(const struct ove_spi_cs *cs,
				       const struct ove_spi_xfer *xfers, unsigned int num_xfers,
				       std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_spi_transfer_seq(handle_, cs, xfers, num_xfers, to_timeout_ns(timeout));
	}

	/** @brief Returns the underlying C handle. */
	ove_spi_t handle() const
	{
		return handle_;
	}

      private:
	ove_spi_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_spi_storage_t storage_{};
#endif
};

} /* namespace ove */

#endif /* CONFIG_OVE_SPI */
