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
 * @note `set_bits_from_isr()` is safe to call from interrupt context.
 */
class EventGroup
{
      public:
	/**
	 * @brief Constructs and initialises the event group with all bits cleared.
	 *
	 * Asserts at startup if initialisation fails.
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
	 * @return The value of the event group after the bits were set.
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
	 * @return The value of the event group before the bits were cleared.
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
	 *         `unexpected` @ref Error (`Error::Timeout` on deadline).
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
	 * @param[in] flags    Wait flags (e.g., @c OVE_EVENT_WAIT_ALL).
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
	 * @return The value of the event group after the bits were set.
	 *
	 * `[[nodiscard]]` for the same reason as @ref set_bits — the
	 * post-set state is the observable result.
	 */
	[[nodiscard]] ove_eventbits_t set_bits_from_isr(ove_eventbits_t bits)
	{
		return ove_eventgroup_set_bits_from_isr(handle_, bits);
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
