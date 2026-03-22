/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file watchdog.hpp
 * @brief Hardware watchdog timer with RAII lifecycle
 */

#pragma once

#ifdef CONFIG_OVE_WATCHDOG

#include <ove/watchdog.h>
#include <ove/types.hpp>

namespace ove {

/**
 * @class Watchdog
 * @brief RAII wrapper around an oveRTOS hardware watchdog timer.
 *
 * The watchdog resets the system if `feed()` is not called within the
 * configured timeout.  Use `start()` to arm the watchdog after construction.
 * The destructor stops and releases the underlying kernel resource.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 */
class Watchdog {
public:
	/**
	 * @brief Constructs and initialises the watchdog with the given timeout.
	 * @param[in] timeout_ms Watchdog timeout in milliseconds.
	 *
	 * Asserts at startup if initialisation fails.
	 */
	explicit Watchdog(uint32_t timeout_ms) {
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_watchdog_init(&handle_, &storage_,
						 timeout_ms);
#else
		int err = ove_watchdog_create(&handle_, timeout_ms);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the watchdog, stopping it and releasing the kernel resource.
	 */
	~Watchdog() {
		if (!handle_) return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_watchdog_deinit(handle_);
#else
		ove_watchdog_destroy(handle_);
#endif
	}

	Watchdog(const Watchdog &) = delete;
	Watchdog &operator=(const Watchdog &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Watchdog(Watchdog &&) = delete;
	Watchdog &operator=(Watchdog &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Watchdog(Watchdog &&other) noexcept : handle_(other.handle_) {
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Watchdog &operator=(Watchdog &&other) noexcept {
		if (this != &other) {
			if (handle_) ove_watchdog_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Arms the watchdog and starts the countdown.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int start() {
		return ove_watchdog_start(handle_);
	}

	/**
	 * @brief Disarms the watchdog, stopping the countdown.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int stop() {
		return ove_watchdog_stop(handle_);
	}

	/**
	 * @brief Resets the watchdog countdown, preventing a system reset.
	 *
	 * Must be called within the configured timeout period.
	 *
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int feed() {
		return ove_watchdog_feed(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the watchdog was successfully initialised.
	 */
	bool valid() const { return handle_ != nullptr; }

	/**
	 * @brief Returns the raw oveRTOS watchdog handle.
	 * @return The opaque `ove_watchdog_t` handle.
	 */
	ove_watchdog_t handle() const { return handle_; }

private:
	ove_watchdog_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_watchdog_storage_t storage_ = {};
#endif
};

} /* namespace ove */

#endif /* CONFIG_OVE_WATCHDOG */
