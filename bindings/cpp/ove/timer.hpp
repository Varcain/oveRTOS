/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file timer.hpp
 * @brief Software timer with RAII lifecycle
 */

#pragma once

#include <ove/timer.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_TIMER

namespace ove
{

/**
 * @brief Concept satisfied by any callable convertible to `ove_timer_fn`.
 *
 * Used to constrain the callback template parameter of `Timer` so that only
 * compatible function-pointer types are accepted.
 *
 * @tparam F The callable type to check.
 */
template <typename F>
concept TimerCallback = std::convertible_to<F, ove_timer_fn>;

/**
 * @class Timer
 * @brief RAII wrapper around an oveRTOS software timer.
 *
 * A software timer calls a user-provided callback either once (`one_shot`)
 * or periodically at the specified interval.  The timer is created in the
 * stopped state; call `start()` to activate it.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 * @note `start()`, `stop()`, and `reset()` are marked `[[nodiscard]]` because
 *       their return value indicates whether the RTOS accepted the request.
 */
class Timer
{
      public:
	/**
	 * @brief Constructs and initialises the timer.
	 *
	 * The timer is created in the stopped state.
	 *
	 * @tparam F        Callable type satisfying `TimerCallback`.
	 * @param[in] callback  Function pointer called when the timer fires.
	 * @param[in] user_data Opaque pointer forwarded to the callback.
	 * @param[in] period_ms Timer period in milliseconds.
	 * @param[in] one_shot  If `true`, the timer fires once and stops; if `false`,
	 *                      it repeats indefinitely.
	 *
	 * Asserts at startup if initialisation fails.
	 */
	template <typename F>
	Timer(F callback, void *user_data, uint32_t period_ms, bool one_shot = false)
		requires TimerCallback<F>
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_timer_init(&handle_, &storage_, callback, user_data, period_ms,
					 one_shot ? 1 : 0);
#else
		int err = ove_timer_create(&handle_, callback, user_data, period_ms,
					   one_shot ? 1 : 0);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the timer, stopping it if running and releasing the kernel resource.
	 */
	~Timer() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_timer_deinit(handle_);
#else
		ove_timer_destroy(handle_);
#endif
	}

	Timer(const Timer &) = delete;
	Timer &operator=(const Timer &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Timer(Timer &&) = delete;
	Timer &operator=(Timer &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Timer(Timer &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Timer &operator=(Timer &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_timer_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Starts the timer.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> start() noexcept
	{
		return from_rc(ove_timer_start(handle_));
	}

	/**
	 * @brief Stops the timer without resetting its period.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> stop() noexcept
	{
		return from_rc(ove_timer_stop(handle_));
	}

	/**
	 * @brief Restarts the timer, resetting the period countdown.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> reset() noexcept
	{
		return from_rc(ove_timer_reset(handle_));
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the timer was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS timer handle.
	 * @return The opaque `ove_timer_t` handle.
	 */
	ove_timer_t handle() const
	{
		return handle_;
	}

      private:
	ove_timer_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_timer_storage_t storage_ = {};
#endif
};

} /* namespace ove */

#endif /* CONFIG_OVE_TIMER */
