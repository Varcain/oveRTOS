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
	 * @brief Sends an item to the back of the queue from task context.
	 * @param[in] item       The item to enqueue (copied into the queue).
	 * @param[in] timeout_ns Maximum time to wait if the queue is full; use
	 *            `OVE_WAIT_FOREVER` to block indefinitely.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int send(const T &item, std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_queue_send(handle_, &item, to_timeout_ns(timeout));
	}

	/**
	 * @brief Deadline-based variant of @ref send.
	 * @param[in] item     The item to enqueue (copied into the queue).
	 * @param[in] deadline @ref ove::steady_clock::time_point at which the
	 *                     wait must complete.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int send_until(const T &item, steady_clock::time_point deadline)
	{
		return ove_queue_send_until(handle_, &item, to_deadline_ns(deadline));
	}

	/**
	 * @brief Receives an item from the front of the queue from task context.
	 * @param[out] item      Pointer to storage for the received item.
	 * @param[in]  timeout_ns Maximum time to wait if the queue is empty; use
	 *             `OVE_WAIT_FOREVER` to block indefinitely.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int receive(T *item, std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_queue_receive(handle_, item, to_timeout_ns(timeout));
	}

	/**
	 * @brief Deadline-based variant of @ref receive.
	 * @param[out] item     Pointer to storage for the received item.
	 * @param[in]  deadline @ref ove::steady_clock::time_point at which the
	 *                      wait must complete.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int receive_until(T *item, steady_clock::time_point deadline)
	{
		return ove_queue_receive_until(handle_, item, to_deadline_ns(deadline));
	}

	/**
	 * @brief Sends an item to the queue from an ISR context (non-blocking).
	 * @param[in] item The item to enqueue.
	 * @return `OVE_OK` on success, or a negative error code if the queue is full.
	 */
	[[nodiscard]] int send_from_isr(const T &item)
	{
		return ove_queue_send_from_isr(handle_, &item);
	}

	/**
	 * @brief Receives an item from the queue from an ISR context (non-blocking).
	 * @param[out] item Pointer to storage for the received item.
	 * @return `OVE_OK` on success, or a negative error code if the queue is empty.
	 */
	[[nodiscard]] int receive_from_isr(T *item)
	{
		return ove_queue_receive_from_isr(handle_, item);
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
