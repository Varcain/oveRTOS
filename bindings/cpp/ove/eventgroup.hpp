/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file eventgroup.hpp
 * @brief Multi-bit event group with RAII lifecycle
 *
 * On NuttX, positive finite waits round up to ticks and saturate at
 * min(WDOG_MAX_DELAY - 2, INT32_MAX - 1) ticks: 1,073,741,821 with
 * 32-bit clocks or 2,147,483,646 with 64-bit clocks. Timeout and stream
 * short counts refer to this effective budget. Only explicit FOREVER
 * waits are indefinite; native tick phase, scheduling and condition-variable
 * mutex reacquisition can extend the elapsed time.
 *
 * FreeRTOS synchronization, queue, EventGroup and stream calls convert a
 * positive finite encoded budget with exact ceil(ns * Hz / 1e9), saturating
 * at portMAX_DELAY - 1 before tick-type narrowing. Encoded FOREVER selects
 * portMAX_DELAY; indefinite native waiting requires INCLUDE_vTaskSuspend=1.
 * These are native-call budgets: scheduling, composed waits and mutex
 * reacquisition can extend elapsed time.
 */

#pragma once

#include <ove/eventgroup.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_EVENTGROUP

namespace ove
{

/**
 * @class EventGroup
 * @brief RAII wrapper around an oveRTOS event-group (bit-field synchronisation object).
 *
 * An event group holds a set of event bits that tasks and ISRs can set,
 * clear, and wait on.  Multiple bits can be waited on simultaneously with
 * optional all-bits-set or any-bit-set semantics, controlled by the `flags`
 * parameter of `wait_bits()`.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 * @note `set_bits_from_isr()` and `clear_bits_from_isr()` are safe to call
 *       from interrupt context.
 */
class EventGroup
{
      public:
	/**
	 * @brief Returns the configured payload mask without accessing an object.
	 * Use this for clear-all and wait-for-any-bit operations.
	 */
	[[nodiscard]] static ove_eventbits_t valid_mask() noexcept
	{
		return ove_eventgroup_valid_mask();
	}

	/**
	 * @brief Constructs and initialises the event group with all bits cleared.
	 *
	 * Asserts at startup if initialisation fails. Native initialization failure
	 * publishes no handle and releases successfully initialized resources.
	 */
	EventGroup()
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_eventgroup_init(&handle_, &storage_);
#else
		int err = ove_eventgroup_create(&handle_);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the event group, releasing the underlying kernel resource.
	 * External producers and synchronous users must be quiescent. FreeRTOS
	 * cancels queued commands before inline or heap backing expires.
	 */
	~EventGroup() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_eventgroup_deinit(handle_);
#else
		ove_eventgroup_destroy(handle_);
#endif
	}

	EventGroup(const EventGroup &) = delete;
	EventGroup &operator=(const EventGroup &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	EventGroup(EventGroup &&) = delete;
	EventGroup &operator=(EventGroup &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	EventGroup(EventGroup &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	EventGroup &operator=(EventGroup &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_eventgroup_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Sets one or more event bits atomically.
	 * @param[in] bits Bitmask of bits to set.
	 * @return The post-set value, or zero without effects for bits outside
	 *         valid_mask(). A real zero snapshot has the same scalar value.
	 *         Setting zero preserves the snapshot without notifying.
	 *
	 * `[[nodiscard]]` because the post-set bit state is the only way to
	 * observe whether your bits actually went on (vs. were cleared by
	 * a concurrent wait_bits with `clear_on_exit`).  Cast to `(void)`
	 * if you genuinely don't care.
	 */
	[[nodiscard]] ove_eventbits_t set_bits(ove_eventbits_t bits)
	{
		return ove_eventgroup_set_bits(handle_, bits);
	}

	/**
	 * @brief Clears one or more event bits atomically.
	 * @param[in] bits Bitmask of bits to clear.
	 * @return The full prior value, or zero without effects for bits outside
	 *         valid_mask(). Clearing zero returns the unchanged prior value.
	 *
	 * `[[nodiscard]]` because the pre-clear state lets callers detect
	 * which bits were actually set at the moment of clearing — useful
	 * for race-free "consume" patterns.
	 */
	[[nodiscard]] ove_eventbits_t clear_bits(ove_eventbits_t bits)
	{
		return ove_eventgroup_clear_bits(handle_, bits);
	}

	/**
	 * @brief Waits until the specified event bits are set, or until the timeout expires.
	 * @param[in] bits    Bitmask of bits to wait for.
	 * @param[in] flags   Wait flags (e.g., wait-for-all vs. wait-for-any).
	 * @param[in] timeout Maximum time to wait.
	 * @return On success, the event-group value at the moment the
	 *         wait condition was satisfied.  On failure, an
	 *         `unexpected` @ref Error (`Error::Timeout` on deadline,
	 *         `Error::InvalidParam` for zero/invalid masks or unknown flags).
	 */
	[[nodiscard]] Result<ove_eventbits_t> wait_bits(ove_eventbits_t bits, uint32_t flags,
							std::chrono::nanoseconds timeout) noexcept
	{
		ove_eventbits_t result = 0;
		const int rc = ove_eventgroup_wait_bits(handle_, bits, flags,
							to_timeout_ns(timeout), &result);
		return from_rc(rc, result);
	}

	/**
	 * @brief Deadline-based variant of @ref wait_bits.
	 * @param[in] bits     Bitmask to wait on.
	 * @param[in] flags    Wait flags (e.g., @c OVE_EG_WAIT_ALL).
	 * @param[in] deadline @ref ove::steady_clock::time_point at which
	 *                     the wait must complete.
	 * @return As @ref wait_bits — `Result<ove_eventbits_t>`.
	 */
	[[nodiscard]] Result<ove_eventbits_t>
	wait_bits_until(ove_eventbits_t bits, uint32_t flags,
			steady_clock::time_point deadline) noexcept
	{
		ove_eventbits_t result = 0;
		const int rc = ove_eventgroup_wait_bits_until(handle_, bits, flags,
							      to_deadline_ns(deadline), &result);
		return from_rc(rc, result);
	}

	/**
	 * @brief Sets one or more event bits from an ISR context.
	 * @param[in] bits Bitmask of bits to set.
	 * @return On FreeRTOS, the accepted requested mask or zero on queue failure;
	 *         publication is deferred. Other engines return the post-set snapshot.
	 *         Invalid bits outside valid_mask() return zero without effects.
	 *
	 * `[[nodiscard]]` because zero can report rejected admission. FreeRTOS
	 * teardown cancels pending commands before backing expires.
	 */
	[[nodiscard]] ove_eventbits_t set_bits_from_isr(ove_eventbits_t bits)
	{
		return ove_eventgroup_set_bits_from_isr(handle_, bits);
	}

	/**
	 * @brief Clears one or more event bits from an ISR context.
	 *
	 * On FreeRTOS the clear is deferred to the timer service task.
	 * @param[in] bits Bitmask of bits to clear.
	 * @return Empty `Result<void>` on success; `unexpected` @ref Error
	 *         (QueueFull) if the deferred clear could not be queued, or
	 *         InvalidParam for bits outside valid_mask().
	 */
	[[nodiscard]] Result<void> clear_bits_from_isr(ove_eventbits_t bits) noexcept
	{
		return from_rc(ove_eventgroup_clear_bits_from_isr(handle_, bits));
	}

	/**
	 * @brief Returns the current value of all event bits without blocking.
	 * @return The current event-group bitmask.
	 */
	ove_eventbits_t get_bits() const
	{
		return ove_eventgroup_get_bits(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the event group was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS event-group handle.
	 * @return The opaque `ove_eventgroup_t` handle.
	 */
	ove_eventgroup_t handle() const
	{
		return handle_;
	}

      private:
	ove_eventgroup_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_eventgroup_storage_t storage_ = {};
#endif
};

} // namespace ove

#endif /* CONFIG_OVE_EVENTGROUP */
