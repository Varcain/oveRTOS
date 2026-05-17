/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file stream.hpp
 * @brief Byte-stream ring buffer with RAII lifecycle
 */

#pragma once

#ifdef CONFIG_OVE_STREAM

#include <ove/stream.h>
#include <ove/types.hpp>

namespace ove
{

/**
 * @class Stream
 * @brief RAII wrapper around an oveRTOS byte-stream (ring-buffer) object.
 *
 * A stream provides a producer/consumer byte buffer with configurable
 * capacity and a watermark `trigger` level.  Receivers block until at least
 * `trigger` bytes are available; senders block if the buffer is full.
 *
 * In zero-heap mode the buffer is stored inline in the wrapper.
 *
 * @tparam BufSize Compile-time buffer capacity in bytes (must be > 0).
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 */
template <size_t BufSize = 0> class Stream
{
      public:
	/**
	 * @brief Constructs and initialises the stream with the given receive trigger.
	 *
	 * Only participates in overload resolution when `BufSize > 0`.
	 *
	 * @param[in] trigger Minimum number of bytes that must be available before
	 *                    a blocked receiver is woken.
	 *
	 * Asserts at startup if initialisation fails.
	 */
	explicit Stream(size_t trigger)
		requires(BufSize > 0)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		static_assert(BufSize > 0, "BufSize must be > 0 in zero-heap mode");
		int err = ove_stream_init(&handle_, &storage_, buffer_, BufSize, trigger);
#else
		int err = ove_stream_create(&handle_, BufSize, trigger);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the stream, releasing the underlying kernel resource.
	 */
	~Stream()
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_stream_deinit(handle_);
#else
		ove_stream_destroy(handle_);
#endif
	}

	Stream(const Stream &) = delete;
	Stream &operator=(const Stream &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Stream(Stream &&) = delete;
	Stream &operator=(Stream &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Stream(Stream &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Stream &operator=(Stream &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_stream_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Sends bytes into the stream from task context.
	 * @param[in]  data       Pointer to the data to send.
	 * @param[in]  len        Number of bytes to send.
	 * @param[in]  timeout_ns Maximum time to wait if the buffer is full.
	 * @param[out] bytes_sent Receives the number of bytes actually written.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int send(const void *data, size_t len, std::chrono::nanoseconds timeout,
			       size_t *bytes_sent)
	{
		return ove_stream_send(handle_, data, len, to_timeout_ns(timeout), bytes_sent);
	}

	/**
	 * @brief Deadline-based variant of @ref send.
	 * @param[in]  data       Pointer to the data to send.
	 * @param[in]  len        Number of bytes to send.
	 * @param[in]  deadline   @ref ove::steady_clock::time_point at which
	 *                        the wait must complete.
	 * @param[out] bytes_sent Receives the number of bytes actually written.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int send_until(const void *data, size_t len,
				     steady_clock::time_point deadline,
				     size_t *bytes_sent)
	{
		return ove_stream_send_until(handle_, data, len, to_deadline_ns(deadline),
					     bytes_sent);
	}

	/**
	 * @brief Receives bytes from the stream from task context.
	 * @param[out] buf            Buffer to receive the data.
	 * @param[in]  len            Maximum number of bytes to read.
	 * @param[in]  timeout_ns     Maximum time to wait for data.
	 * @param[out] bytes_received Receives the number of bytes actually read.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int receive(void *buf, size_t len, std::chrono::nanoseconds timeout,
				  size_t *bytes_received)
	{
		return ove_stream_receive(handle_, buf, len, to_timeout_ns(timeout), bytes_received);
	}

	/**
	 * @brief Deadline-based variant of @ref receive.
	 * @param[out] buf            Buffer to receive the data.
	 * @param[in]  len            Maximum number of bytes to read.
	 * @param[in]  deadline       @ref ove::steady_clock::time_point at which
	 *                            the wait must complete.
	 * @param[out] bytes_received Receives the number of bytes actually read.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int receive_until(void *buf, size_t len, steady_clock::time_point deadline,
					size_t *bytes_received)
	{
		return ove_stream_receive_until(handle_, buf, len, to_deadline_ns(deadline),
						bytes_received);
	}

	/**
	 * @brief Sends bytes into the stream from an ISR context (non-blocking).
	 * @param[in]  data       Pointer to the data to send.
	 * @param[in]  len        Number of bytes to send.
	 * @param[out] bytes_sent Receives the number of bytes actually written.
	 * @return `OVE_OK` on success, or a negative error code if the buffer is full.
	 */
	[[nodiscard]] int send_from_isr(const void *data, size_t len, size_t *bytes_sent)
	{
		return ove_stream_send_from_isr(handle_, data, len, bytes_sent);
	}

	/**
	 * @brief Receives bytes from the stream from an ISR context (non-blocking).
	 * @param[out] buf            Buffer to receive the data.
	 * @param[in]  len            Maximum number of bytes to read.
	 * @param[out] bytes_received Receives the number of bytes actually read.
	 * @return `OVE_OK` on success, or a negative error code if insufficient data.
	 */
	[[nodiscard]] int receive_from_isr(void *buf, size_t len, size_t *bytes_received)
	{
		return ove_stream_receive_from_isr(handle_, buf, len, bytes_received);
	}

	/**
	 * @brief Resets the stream, discarding any buffered data.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int reset()
	{
		return ove_stream_reset(handle_);
	}

	/**
	 * @brief Returns the number of bytes currently available to read.
	 * @return Number of readable bytes in the stream buffer.
	 */
	size_t bytes_available() const
	{
		return ove_stream_bytes_available(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the stream was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS stream handle.
	 * @return The opaque `ove_stream_t` handle.
	 */
	ove_stream_t handle() const
	{
		return handle_;
	}

      private:
	ove_stream_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_stream_storage_t storage_ = {};
	uint8_t buffer_[(BufSize > 0 ? BufSize : 1) + 1];
#endif
};

} /* namespace ove */

#endif /* CONFIG_OVE_STREAM */
