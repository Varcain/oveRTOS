/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file types.hpp
 * @brief Common type definitions and concepts for the C++ wrapper layer
 */

#pragma once

#include <ove/ove.h>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <concepts>
#include <type_traits>
#include <utility>

/**
 * @namespace ove
 * @brief Top-level namespace for all oveRTOS C++ abstractions.
 *
 * The `ove` namespace provides C++20 RAII wrappers around the oveRTOS C API.
 * All sync primitives, threads, queues, timers, and peripheral helpers live
 * here.  Nested namespaces (`ove::console`, `ove::time`, `ove::gpio`, etc.)
 * group thin inline wrappers around optional subsystem APIs that are enabled
 * by their corresponding `CONFIG_OVE_*` Kconfig options.
 *
 * @note All classes in this namespace are non-copyable.  Move semantics are
 *       available unless `CONFIG_OVE_ZERO_HEAP` is set, in which case the
 *       underlying kernel object lives in a member storage buffer and the
 *       address of that buffer must remain stable for the lifetime of the
 *       wrapper.
 */
namespace ove
{

/* ------------------------------------------------------------------ */
/*  C++20 Concepts                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Concept satisfied by any callable convertible to `void(*)(void*)`.
 *
 * Used to constrain the entry-function template parameter of `Thread` so
 * that only stateless function pointers or compatible functors are accepted.
 *
 * @tparam F The callable type to check.
 */
template <typename F>
concept ThreadEntry = std::convertible_to<F, void (*)(void *)>;

/* ------------------------------------------------------------------ */
/*  Compile-time C-ABI shape check                                    */
/* ------------------------------------------------------------------ */
/*
 * Pin the OVE_ERR_* numeric values at compile time.  Mirrors Rust's
 * `_assert_codes_match()` (bindings/rust/ove/src/error.rs:127-150)
 * and Zig's `comptime { std.debug.assert(...) }` block
 * (bindings/zig/ove/src/error.zig:88-102).  If a future C-header
 * rename or renumber drifts an OVE_ERR_* value, every C++ TU that
 * includes <ove/types.hpp> (i.e. every TU in apps/cpp and the C++
 * binding) fails to build with a clear message — the silent-drift
 * surface that bit Rust's mangled-name audit is closed at the C++
 * boundary too.
 */
static_assert(OVE_ERR_NOT_REGISTERED == -1, "OVE_ERR_NOT_REGISTERED drifted");
static_assert(OVE_ERR_INVALID_PARAM == -2, "OVE_ERR_INVALID_PARAM drifted");
static_assert(OVE_ERR_NO_MEMORY == -3, "OVE_ERR_NO_MEMORY drifted");
static_assert(OVE_ERR_TIMEOUT == -4, "OVE_ERR_TIMEOUT drifted");
static_assert(OVE_ERR_NOT_SUPPORTED == -5, "OVE_ERR_NOT_SUPPORTED drifted");
static_assert(OVE_ERR_QUEUE_FULL == -6, "OVE_ERR_QUEUE_FULL drifted");
static_assert(OVE_ERR_ML_FAILED == -7, "OVE_ERR_ML_FAILED drifted");
static_assert(OVE_ERR_NET_REFUSED == -8, "OVE_ERR_NET_REFUSED drifted");
static_assert(OVE_ERR_NET_UNREACHABLE == -9, "OVE_ERR_NET_UNREACHABLE drifted");
static_assert(OVE_ERR_NET_ADDR_IN_USE == -10, "OVE_ERR_NET_ADDR_IN_USE drifted");
static_assert(OVE_ERR_NET_RESET == -11, "OVE_ERR_NET_RESET drifted");
static_assert(OVE_ERR_NET_DNS_FAIL == -12, "OVE_ERR_NET_DNS_FAIL drifted");
static_assert(OVE_ERR_NET_CLOSED == -13, "OVE_ERR_NET_CLOSED drifted");
static_assert(OVE_ERR_BUS_NACK == -14, "OVE_ERR_BUS_NACK drifted");
static_assert(OVE_ERR_BUS_BUSY == -15, "OVE_ERR_BUS_BUSY drifted");
static_assert(OVE_ERR_BUS_ERROR == -16, "OVE_ERR_BUS_ERROR drifted");
static_assert(OVE_ERR_QUEUE_EMPTY == -17, "OVE_ERR_QUEUE_EMPTY drifted");
static_assert(OVE_ERR_WOULD_BLOCK == -18, "OVE_ERR_WOULD_BLOCK drifted");
static_assert(OVE_ERR_EOF == -19, "OVE_ERR_EOF drifted");
static_assert(OVE_ERR_INVAL == -20, "OVE_ERR_INVAL drifted");
static_assert(OVE_ERR_NOT_FOUND == -21, "OVE_ERR_NOT_FOUND drifted");
static_assert(OVE_ERR_NET_ADDR_NOT_AVAILABLE == -22, "OVE_ERR_NET_ADDR_NOT_AVAILABLE drifted");
static_assert(OVE_ERR_ALREADY_EXISTS == -23, "OVE_ERR_ALREADY_EXISTS drifted");
static_assert(OVE_ERR_NO_SPACE == -24, "OVE_ERR_NO_SPACE drifted");
static_assert(OVE_ERR_NOT_DIR == -25, "OVE_ERR_NOT_DIR drifted");
static_assert(OVE_ERR_IS_DIR == -26, "OVE_ERR_IS_DIR drifted");
static_assert(OVE_ERR_NOT_EMPTY == -27, "OVE_ERR_NOT_EMPTY drifted");
static_assert(OVE_ERR_READ_ONLY == -28, "OVE_ERR_READ_ONLY drifted");
static_assert(OVE_ERR_IO == -29, "OVE_ERR_IO drifted");
static_assert(OVE_ERR_BUSY == -30, "OVE_ERR_BUSY drifted");
static_assert(OVE_ERR_NAME_TOO_LONG == -31, "OVE_ERR_NAME_TOO_LONG drifted");
static_assert(OVE_ERR_BAD_HANDLE == -32, "OVE_ERR_BAD_HANDLE drifted");
static_assert(OVE_ERR_PERMISSION == -33, "OVE_ERR_PERMISSION drifted");
static_assert(OVE_ERR_CROSS_DEVICE == -34, "OVE_ERR_CROSS_DEVICE drifted");

/* ------------------------------------------------------------------ */
/*  Duration / timeout types                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Sentinel duration meaning "block indefinitely".
 *
 * Represented as @c std::chrono::nanoseconds::max() (≈ 292 years; signed
 * @c int64_t representation).  @c to_timeout_ns recognises this exact
 * value and emits the C-side @c OVE_WAIT_FOREVER (= @c UINT64_MAX) sentinel.
 *
 * Primitives split forever-blocking from bounded-wait into separate
 * methods — the forever form takes no timeout argument; the bounded form
 * takes a @c std::chrono::duration.  Pass `wait_forever` to the C-style
 * helpers that still accept a single @c nanoseconds timeout (network
 * sockets, I2C/SPI/UART transfers, …).
 *
 * @code
 * using namespace std::chrono_literals;
 * (void)queue.try_send_for(item, 100ms);  // bounded-wait form returns Result<void>
 * queue.send(item);                       // forever-blocking form returns void
 * mtx.lock();                             // Mutex uses void-return indefinite form
 * sock.recv(buf, sizeof(buf), ove::wait_forever);  // socket recv takes the sentinel
 * @endcode
 */
inline constexpr std::chrono::nanoseconds wait_forever = std::chrono::nanoseconds::max();

/**
 * @brief Convert a chrono duration to @c uint64_t nanoseconds for the C API.
 *
 * Used internally by every wrapper that calls a substrate function taking
 * @c uint64_t timeout_ns. Saturates to 0 on negative durations and maps
 * @c wait_forever (== @c nanoseconds::max()) to @c OVE_WAIT_FOREVER.
 */
template <typename Rep, typename Period>
inline constexpr uint64_t to_timeout_ns(std::chrono::duration<Rep, Period> d) noexcept
{
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d);
	if (ns.count() < 0)
		return 0u;
	if (ns == std::chrono::nanoseconds::max())
		return OVE_WAIT_FOREVER;
	return static_cast<uint64_t>(ns.count());
}

/**
 * @brief Steady-clock wrapping the substrate's monotonic time source.
 *
 * Satisfies the C++ `TrivialClock` requirements so it can be used
 * with `std::chrono` arithmetic.  The epoch matches whatever
 * @ref ove_time_now_steady_ns reports — opaque, monotonic, ns-resolved.
 *
 * Use with the binding's `_until` variants:
 * @code
 * auto deadline = ove::steady_clock::now() + 100ms;
 * if (mtx.try_lock_until(deadline).has_value()) {
 *     // …critical section…
 *     mtx.unlock();
 * }
 * @endcode
 */
struct steady_clock {
	using duration = std::chrono::nanoseconds;
	using rep = duration::rep;
	using period = duration::period;
	using time_point = std::chrono::time_point<steady_clock>;
	static constexpr bool is_steady = true;

	static time_point now() noexcept
	{
		return time_point(duration(ove_time_now_steady_ns()));
	}
};

/**
 * @brief Convert an @ref ove::steady_clock::time_point to @c uint64_t
 *        nanoseconds for the substrate's `_until` APIs.
 *
 * Preserves the `time_point` whose duration equals
 * @c std::chrono::nanoseconds::max() as the @c OVE_WAIT_FOREVER sentinel
 * so passing @c steady_clock::time_point::max() blocks indefinitely.
 */
inline constexpr uint64_t to_deadline_ns(steady_clock::time_point tp) noexcept
{
	const auto since_epoch = tp.time_since_epoch();
	if (since_epoch == std::chrono::nanoseconds::max())
		return OVE_WAIT_FOREVER;
	if (since_epoch.count() < 0)
		return 0u;
	return static_cast<uint64_t>(since_epoch.count());
}

} // namespace ove
