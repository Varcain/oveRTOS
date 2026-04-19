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

namespace ove {

/**
 * @class Spi
 * @brief RAII wrapper around an oveRTOS SPI bus controller.
 *
 * Not copyable.  Move-only when heap allocation is enabled.
 */
class Spi {
public:
	explicit Spi(const struct ove_spi_cfg &cfg) {
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_spi_init(&handle_, &storage_, &cfg);
#else
		int err = ove_spi_create(&handle_, &cfg);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	~Spi() {
		if (!handle_) return;
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
	Spi(Spi &&o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
	Spi &operator=(Spi &&o) noexcept {
		if (this != &o) {
			if (handle_) ove_spi_destroy(handle_);
			handle_ = o.handle_;
			o.handle_ = nullptr;
		}
		return *this;
	}
#endif

	[[nodiscard]] int transfer(const struct ove_spi_cs *cs,
				   const void *tx, void *rx, size_t len,
				   uint32_t timeout_ms = OVE_WAIT_FOREVER) {
		return ove_spi_transfer(handle_, cs, tx, rx, len, timeout_ms);
	}

	[[nodiscard]] int write(const struct ove_spi_cs *cs,
				const void *data, size_t len,
				uint32_t timeout_ms = OVE_WAIT_FOREVER) {
		return ove_spi_write(handle_, cs, data, len, timeout_ms);
	}

	[[nodiscard]] int read(const struct ove_spi_cs *cs,
			       void *buf, size_t len,
			       uint32_t timeout_ms = OVE_WAIT_FOREVER) {
		return ove_spi_read(handle_, cs, buf, len, timeout_ms);
	}

	[[nodiscard]] int transfer_seq(const struct ove_spi_cs *cs,
				       const struct ove_spi_xfer *xfers,
				       unsigned int num_xfers,
				       uint32_t timeout_ms = OVE_WAIT_FOREVER) {
		return ove_spi_transfer_seq(handle_, cs, xfers, num_xfers,
					    timeout_ms);
	}

	ove_spi_t handle() const { return handle_; }

private:
	ove_spi_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_spi_storage_t storage_{};
#endif
};

} /* namespace ove */

#endif /* CONFIG_OVE_SPI */
