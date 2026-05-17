/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file uart.hpp
 * @brief UART serial bus with RAII lifecycle
 */

#pragma once

#ifdef CONFIG_OVE_UART

#include <ove/uart.h>
#include <ove/types.hpp>

namespace ove
{

/**
 * @class Uart
 * @brief RAII wrapper around an oveRTOS UART peripheral.
 *
 * In zero-heap mode the RX buffer is stored inline.
 *
 * @tparam RxBufSize Compile-time RX buffer capacity in bytes.
 *
 * Not copyable.  Move-only when heap allocation is enabled.
 */
template <size_t RxBufSize = 0> class Uart
{
      public:
	/**
	 * @brief Construct and initialise a UART port from `cfg`.
	 * @param[in] cfg Port configuration (baud, parity, stop bits, flow control).
	 */
	explicit Uart(const struct ove_uart_cfg &cfg)
		requires(RxBufSize > 0)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		static_assert(RxBufSize > 0, "RxBufSize must be > 0 in zero-heap mode");
		int err = ove_uart_init(&handle_, &storage_, rx_buf_, &cfg);
#else
		int err = ove_uart_create(&handle_, &cfg);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	~Uart()
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_uart_deinit(handle_);
#else
		ove_uart_destroy(handle_);
#endif
	}

	Uart(const Uart &) = delete;
	Uart &operator=(const Uart &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Uart(Uart &&) = delete;
	Uart &operator=(Uart &&) = delete;
#else
	/** @brief Move constructor — transfers handle; source becomes empty. */
	Uart(Uart &&o) noexcept : handle_(o.handle_)
	{
		o.handle_ = nullptr;
	}
	/** @brief Move-assignment — destroys current port, then takes `o`'s handle. */
	Uart &operator=(Uart &&o) noexcept
	{
		if (this != &o) {
			if (handle_)
				ove_uart_destroy(handle_);
			handle_ = o.handle_;
			o.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/** @brief Write bytes to the port; `bytes_written` optionally receives the count. */
	[[nodiscard]] int write(const void *data, size_t len,
				std::chrono::nanoseconds timeout = wait_forever,
				size_t *bytes_written = nullptr)
	{
		return ove_uart_write(handle_, data, len, to_timeout_ns(timeout), bytes_written);
	}

	/** @brief Read bytes from the RX buffer; `bytes_read` optionally receives the count. */
	[[nodiscard]] int read(void *buf, size_t len, std::chrono::nanoseconds timeout = wait_forever,
			       size_t *bytes_read = nullptr)
	{
		return ove_uart_read(handle_, buf, len, to_timeout_ns(timeout), bytes_read);
	}

	/** @brief Bytes currently available in the RX buffer. */
	size_t bytes_available() const
	{
		return ove_uart_bytes_available(handle_);
	}

	/** @brief Block until all pending TX bytes have been drained. */
	[[nodiscard]] int flush()
	{
		return ove_uart_flush(handle_);
	}

	/** @brief Returns the underlying C handle. */
	ove_uart_t handle() const
	{
		return handle_;
	}

      private:
	ove_uart_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_uart_storage_t storage_{};
	uint8_t rx_buf_[RxBufSize > 0 ? RxBufSize : 1]{};
#endif
};

} /* namespace ove */

#endif /* CONFIG_OVE_UART */
