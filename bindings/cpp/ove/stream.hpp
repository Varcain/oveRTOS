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
	~Stream() noexcept
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
	 * @brief Sends bytes into the stream, blocking indefinitely.
	 *
	 * Forever-wait form: failure means the handle is unusable.  Aborts
	 * via @c OVE_STATIC_INIT_ASSERT (same pattern as @ref Queue::send).
	 * Even on success, @p bytes_sent may be less than @p len if the
	 * substrate's internal buffer dictates a smaller commit — caller
	 * must inspect @p bytes_sent.
	 *
	 * @param[in]  data       Pointer to the data to send.
	 * @param[in]  len        Number of bytes to send.
	 * @param[out] bytes_sent Number of bytes actually written.
	 */
	void send(const void *data, size_t len, size_t &bytes_sent)
	{
		const int err = ove_stream_send(handle_, data, len, OVE_WAIT_FOREVER, &bytes_sent);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Non-blocking send.
	 * @param[out] bytes_sent Number of bytes actually written
	 *                        (may be < @p len even on success).
	 * @return `true` if any bytes were written, `false` if the buffer was full.
	 */
	[[nodiscard]] bool try_send(const void *data, size_t len, size_t &bytes_sent)
	{
		return ove_stream_send(handle_, data, len, 0, &bytes_sent) == OVE_OK;
	}

	/**
	 * @brief Bounded-wait send.
	 * @return `OVE_OK` on success (with @p bytes_sent set), `OVE_ERR_TIMEOUT`
	 *         on timeout, or a negative error code on backend failure.
	 */
	[[nodiscard]] int try_send_for(const void *data, size_t len,
					std::chrono::nanoseconds rel,
					size_t &bytes_sent)
	{
		return ove_stream_send(handle_, data, len, to_timeout_ns(rel), &bytes_sent);
	}

	/**
	 * @brief Deadline-based send templated over the clock.
	 * See @ref Mutex::try_lock_until for the templated-clock rationale.
	 */
	template <typename Clock, typename Duration>
	[[nodiscard]] int try_send_until(const void *data, size_t len,
					  const std::chrono::time_point<Clock, Duration> &deadline,
					  size_t &bytes_sent)
	{
		const auto rel = deadline - Clock::now();
		return ove_stream_send(handle_, data, len, to_timeout_ns(rel), &bytes_sent);
	}

	/**
	 * @brief Receives bytes from the stream, blocking indefinitely.
	 *
	 * Forever-wait form: failure means the handle is unusable.  Aborts
	 * via @c OVE_STATIC_INIT_ASSERT.  Like @ref send, @p bytes_received
	 * may be less than @p len on success — caller must inspect it.
	 *
	 * @param[out] buf            Buffer to receive the data.
	 * @param[in]  len            Maximum number of bytes to read.
	 * @param[out] bytes_received Number of bytes actually read.
	 */
	void receive(void *buf, size_t len, size_t &bytes_received)
	{
		const int err =
			ove_stream_receive(handle_, buf, len, OVE_WAIT_FOREVER, &bytes_received);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Non-blocking receive.
	 * @return `true` if any bytes were read, `false` if the buffer was empty.
	 */
	[[nodiscard]] bool try_receive(void *buf, size_t len, size_t &bytes_received)
	{
		return ove_stream_receive(handle_, buf, len, 0, &bytes_received) == OVE_OK;
	}

	/**
	 * @brief Bounded-wait receive.
	 */
	[[nodiscard]] int try_receive_for(void *buf, size_t len,
					   std::chrono::nanoseconds rel,
					   size_t &bytes_received)
	{
		return ove_stream_receive(handle_, buf, len, to_timeout_ns(rel), &bytes_received);
	}

	/**
	 * @brief Deadline-based receive templated over the clock.
	 */
	template <typename Clock, typename Duration>
	[[nodiscard]] int try_receive_until(void *buf, size_t len,
					     const std::chrono::time_point<Clock, Duration> &deadline,
					     size_t &bytes_received)
	{
		const auto rel = deadline - Clock::now();
		return ove_stream_receive(handle_, buf, len, to_timeout_ns(rel), &bytes_received);
	}

	/**
	 * @brief Sends bytes into the stream from an ISR context (non-blocking).
	 * @return `OVE_OK` on success, or a negative error code if the buffer is full.
	 */
	[[nodiscard]] int send_from_isr(const void *data, size_t len, size_t &bytes_sent)
	{
		return ove_stream_send_from_isr(handle_, data, len, &bytes_sent);
	}

	/**
	 * @brief Receives bytes from the stream from an ISR context (non-blocking).
	 * @return `OVE_OK` on success, or a negative error code if insufficient data.
	 */
	[[nodiscard]] int receive_from_isr(void *buf, size_t len, size_t &bytes_received)
	{
		return ove_stream_receive_from_isr(handle_, buf, len, &bytes_received);
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
