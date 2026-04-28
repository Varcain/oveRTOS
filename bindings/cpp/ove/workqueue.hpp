/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file workqueue.hpp
 * @brief Deferred work queue and work items with RAII lifecycle
 */

#pragma once

#ifdef CONFIG_OVE_WORKQUEUE

#include <ove/workqueue.h>
#include <ove/types.hpp>

namespace ove
{

/**
 * @brief Concept satisfied by any callable convertible to `ove_work_fn`.
 *
 * Used to constrain the handler template parameter of `Work`.
 *
 * @tparam F The callable type to check.
 */
template <typename F>
concept WorkHandler = std::convertible_to<F, ove_work_fn>;

/**
 * @class Workqueue
 * @brief RAII wrapper around an oveRTOS workqueue (dedicated worker thread).
 *
 * A workqueue owns a worker thread that executes `Work` items submitted to
 * it.  Items are processed serially in FIFO order.
 *
 * @tparam StackSize Stack size in bytes for the worker thread (must be > 0).
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 */
template <size_t StackSize = 0> class Workqueue
{
      public:
	/**
	 * @brief Constructs and starts the workqueue.
	 *
	 * Only participates in overload resolution when `StackSize > 0`.
	 *
	 * @param[in] name Human-readable name for the worker thread.
	 * @param[in] prio Priority of the worker thread.
	 *
	 * Asserts at startup if initialisation fails.
	 */
	Workqueue(const char *name, ove_prio_t prio)
		requires(StackSize > 0)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		static_assert(StackSize > 0, "StackSize must be > 0 in zero-heap mode");
		int err = ove_workqueue_init(&handle_, &storage_, name, prio, StackSize, stack_);
#else
		int err = ove_workqueue_create(&handle_, name, prio, StackSize);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the workqueue and terminates the worker thread.
	 */
	~Workqueue() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_workqueue_deinit(handle_);
#else
		ove_workqueue_destroy(handle_);
#endif
	}

	Workqueue(const Workqueue &) = delete;
	Workqueue &operator=(const Workqueue &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Workqueue(Workqueue &&) = delete;
	Workqueue &operator=(Workqueue &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Workqueue(Workqueue &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Workqueue &operator=(Workqueue &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_workqueue_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the workqueue was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS workqueue handle.
	 * @return The opaque `ove_workqueue_t` handle.
	 */
	ove_workqueue_t handle() const
	{
		return handle_;
	}

      private:
	ove_workqueue_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_workqueue_storage_t storage_ = {};
	OVE_THREAD_STACK_MEMBER_(stack_, StackSize > 0 ? StackSize : 1);
#endif
};

/**
 * @class Work
 * @brief RAII wrapper representing a single deferred work item.
 *
 * A `Work` item encapsulates a handler function and can be submitted to a
 * `Workqueue` for asynchronous or delayed execution.  The same `Work` object
 * must not be submitted while it is already pending or running.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 */
class Work
{
      public:
	/**
	 * @brief Constructs a work item with the given handler function.
	 * @tparam F Handler type satisfying `WorkHandler`.
	 * @param[in] handler Function pointer called when the work item is executed.
	 *
	 * Asserts at startup if initialisation fails.
	 */
	template <typename F>
	explicit Work(F handler)
		requires WorkHandler<F>
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_work_init_static(&handle_, &storage_, handler);
#else
		int err = ove_work_init(&handle_, handler);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the work item, freeing its kernel resource (heap mode).
	 */
	~Work() noexcept
	{
		if (!handle_)
			return;
#ifndef CONFIG_OVE_ZERO_HEAP
		ove_work_free(handle_);
#endif
	}

	Work(const Work &) = delete;
	Work &operator=(const Work &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Work(Work &&) = delete;
	Work &operator=(Work &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Work(Work &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Work &operator=(Work &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_work_free(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Submits the work item to a workqueue for immediate execution.
	 * @tparam S Stack size of the target workqueue.
	 * @param[in] wq The workqueue to submit this work item to.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	template <size_t S> [[nodiscard]] int submit(Workqueue<S> &wq)
	{
		return ove_work_submit(wq.handle(), handle_);
	}

	/**
	 * @brief Submits the work item to a workqueue with a delay.
	 * @tparam S Stack size of the target workqueue.
	 * @param[in] wq       The workqueue to submit this work item to.
	 * @param[in] delay_ms Delay in milliseconds before the item is executed.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	template <size_t S> [[nodiscard]] int submit_delayed(Workqueue<S> &wq, uint32_t delay_ms)
	{
		return ove_work_submit_delayed(wq.handle(), handle_, delay_ms);
	}

	/**
	 * @brief Attempts to cancel a pending work item.
	 *
	 * If the item is already running, cancellation may not be possible.
	 *
	 * @return `OVE_OK` if cancelled, or a negative error code.
	 */
	[[nodiscard]] int cancel()
	{
		return ove_work_cancel(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the work item was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS work handle.
	 * @return The opaque `ove_work_t` handle.
	 */
	ove_work_t handle() const
	{
		return handle_;
	}

      private:
	ove_work_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_work_storage_t storage_ = {};
#endif
};

} /* namespace ove */

#endif /* CONFIG_OVE_WORKQUEUE */
