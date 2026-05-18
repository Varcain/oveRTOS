/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file queue.hpp
 * @brief Type-safe, fixed-depth message queue with RAII lifecycle
 */

#pragma once

#include <ove/queue.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

#include <type_traits>

#ifdef CONFIG_OVE_QUEUE

namespace ove
{

/**
 * @class Queue
 * @brief RAII wrapper around an oveRTOS typed message queue.
 *
 * `Queue<T, MaxItems>` stores items of type `T` in a FIFO buffer.  Items are
 * copied into the queue on `send()` and out of it on `receive()`, so `T`
 * must be trivially copyable (or at least safe to copy via `memcpy`).
 *
 * In zero-heap mode `MaxItems` must be greater than zero; the backing buffer
 * is allocated inside the wrapper.  On heap-enabled builds `MaxItems` is
 * also required at compile time (it is passed to the kernel at construction).
 *
 * @tparam T        Type of message items; should be trivially copyable.
 * @tparam MaxItems Compile-time capacity of the queue (must be > 0).
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 * @note `send()`, `receive()`, and their ISR variants are marked
 *       `[[nodiscard]]`.
 */
template <typename T, size_t MaxItems = 0> class Queue
{
	static_assert(std::is_trivially_copyable_v<T>,
		      "ove::Queue<T, N> requires T to be trivially copyable. "
		      "The substrate memcpy()s items of sizeof(T) bytes — non-trivial "
		      "types (std::string, std::vector, std::unique_ptr, user types with "
		      "explicit copy/move ctors or destructors that do bookkeeping) corrupt "
		      "their internal state on memcpy.  Wrap them in a smart pointer or "
		      "POD struct, or transfer ownership by pointer through the queue.");

      public:
	/**
	 * @brief Constructs and initialises the queue.
	 *
	 * Only participates in overload resolution when `MaxItems > 0`.
	 * Asserts at startup if initialisation fails.
	 */
	Queue()
		requires(MaxItems > 0)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		static_assert(MaxItems > 0, "MaxItems must be > 0 in zero-heap mode");
		int err = ove_queue_init(&handle_, &storage_, buffer_, sizeof(T), MaxItems);
#else
		int err = ove_queue_create(&handle_, sizeof(T), MaxItems);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the queue, releasing the underlying kernel resource.
	 */
	~Queue() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_queue_deinit(handle_);
#else
		ove_queue_destroy(handle_);
#endif
	}

	Queue(const Queue &) = delete;
	Queue &operator=(const Queue &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Queue(Queue &&) = delete;
	Queue &operator=(Queue &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Queue(Queue &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Queue &operator=(Queue &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_queue_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Sends an item to the back of the queue, blocking indefinitely.
	 *
	 * Forever-wait form: failure means the handle is unusable.  Aborts
	 * via @c OVE_STATIC_INIT_ASSERT (same pattern as @ref Mutex::lock,
	 * @ref Semaphore::acquire).
	 */
	void send(const T &item)
	{
		const int err = ove_queue_send(handle_, &item, OVE_WAIT_FOREVER);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Non-blocking send.
	 * @return `true` on success, `false` if the queue was full.
	 */
	[[nodiscard]] bool try_send(const T &item)
	{
		return ove_queue_send(handle_, &item, 0) == OVE_OK;
	}

	/**
	 * @brief Bounded-wait send.
	 *
	 * @param[in] item The item to enqueue (copied into the queue).
	 * @param[in] rel  Relative timeout (any `std::chrono::duration` unit).
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error::QueueFull / @ref Error::Timeout if the
	 *         queue stayed full through the deadline; `unexpected`
	 *         with another @ref Error value on backend failure.
	 */
	template <class Rep, class Period>
	[[nodiscard]] Result<void>
	try_send_for(const T &item, const std::chrono::duration<Rep, Period> &rel) noexcept
	{
		return from_rc(ove_queue_send(handle_, &item, to_timeout_ns(rel)));
	}

	/**
	 * @brief Deadline-based send templated over the clock.  See
	 * @ref Mutex::try_lock_until for the templated-clock rationale.
	 *
	 * @return As @ref try_send_for — `Result<void>` with the
	 *         appropriate @ref Error on timeout / backend failure.
	 */
	template <class Clock, class Duration>
	[[nodiscard]] Result<void>
	try_send_until(const T &item,
		       const std::chrono::time_point<Clock, Duration> &deadline) noexcept
	{
		const auto rel = deadline - Clock::now();
		return from_rc(ove_queue_send(handle_, &item, to_timeout_ns(rel)));
	}

	/**
	 * @brief Receives an item from the front of the queue, blocking indefinitely.
	 *
	 * Forever-wait form: failure means the handle is unusable.  Aborts
	 * via @c OVE_STATIC_INIT_ASSERT.
	 *
	 * @param[out] out Reference to storage for the received item.
	 */
	void receive(T &out)
	{
		const int err = ove_queue_receive(handle_, &out, OVE_WAIT_FOREVER);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Non-blocking receive.
	 * @return `true` on success, `false` if the queue was empty.
	 */
	[[nodiscard]] bool try_receive(T &out)
	{
		return ove_queue_receive(handle_, &out, 0) == OVE_OK;
	}

	/**
	 * @brief Bounded-wait receive.
	 *
	 * @param[out] out Reference to storage for the received item.
	 * @param[in]  rel Relative timeout (any `std::chrono::duration` unit).
	 * @return Empty `Result<void>` on success (item written to @p out);
	 *         `unexpected` @ref Error::QueueEmpty / @ref Error::Timeout
	 *         if the queue stayed empty through the deadline;
	 *         `unexpected` with another @ref Error value on backend
	 *         failure.
	 */
	template <class Rep, class Period>
	[[nodiscard]] Result<void>
	try_receive_for(T &out, const std::chrono::duration<Rep, Period> &rel) noexcept
	{
		return from_rc(ove_queue_receive(handle_, &out, to_timeout_ns(rel)));
	}

	/**
	 * @brief Deadline-based receive templated over the clock.
	 *
	 * @return As @ref try_receive_for.
	 */
	template <class Clock, class Duration>
	[[nodiscard]] Result<void>
	try_receive_until(T &out, const std::chrono::time_point<Clock, Duration> &deadline) noexcept
	{
		const auto rel = deadline - Clock::now();
		return from_rc(ove_queue_receive(handle_, &out, to_timeout_ns(rel)));
	}

	/**
	 * @brief Sends an item to the queue from an ISR context (non-blocking).
	 * @param[in] item The item to enqueue.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error::QueueFull if the queue was full;
	 *         `unexpected` with another @ref Error value on backend
	 *         failure.
	 */
	[[nodiscard]] Result<void> send_from_isr(const T &item) noexcept
	{
		return from_rc(ove_queue_send_from_isr(handle_, &item));
	}

	/**
	 * @brief Receives an item from the queue from an ISR context (non-blocking).
	 * @param[out] out Reference to storage for the received item.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error::QueueEmpty if the queue was empty;
	 *         `unexpected` with another @ref Error value on backend
	 *         failure.
	 */
	[[nodiscard]] Result<void> receive_from_isr(T &out) noexcept
	{
		return from_rc(ove_queue_receive_from_isr(handle_, &out));
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the queue was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS queue handle.
	 * @return The opaque `ove_queue_t` handle.
	 */
	ove_queue_t handle() const
	{
		return handle_;
	}

      private:
	ove_queue_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	/* `buffer_` precedes `storage_` so the latter (whose underlying C
	 * struct ends in a flexible-array `inline_storage[]`) sits at the
	 * end of the C++ class.  C99 allows FAMs anywhere in a struct, but
	 * C++ rejects another member after one — see
	 * backends/freertos/include/ove_storage_freertos.h FAM commentary. */
	T buffer_[MaxItems > 0 ? MaxItems : 1];
	ove_queue_storage_t storage_ = {};
#endif
};

} // namespace ove

#endif /* CONFIG_OVE_QUEUE */
