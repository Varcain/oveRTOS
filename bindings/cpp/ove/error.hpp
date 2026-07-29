/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file error.hpp
 * @brief Strong @ref ove::Error type, `Result<T>` alias, and
 *        `std::error_code` interop for the oveRTOS C++ binding.
 *
 * Provides the foundation for `Result<T>`-shaped fallible operations.
 * Primitives migrate to returning `Result<T>` in their own iterations;
 * for any not-yet-migrated primitive the substrate's `int rc` is
 * still the canonical surface, and @ref from_rc lifts those rc-codes
 * into a `Result` at the call site.
 *
 *   - `enum class Error : int` mirroring every `OVE_ERR_*` substrate
 *     value bit-for-bit (so `static_cast<int>(Error::Timeout)
 *     == OVE_ERR_TIMEOUT`).
 *   - `Result<T> = std::expected<T, Error>` (default `T = void`).
 *   - `error_category()` + `make_error_code(Error)` for
 *     `std::error_code` interop — `Error` round-trips through
 *     generic `std::error_code`-based user code.
 *   - `from_rc(int)` and `from_rc(int, T&&)` to lift substrate
 *     rc-codes into a `Result`.
 *
 * **Drift guard.**  Each `Error` variant is `static_assert`-pinned
 * to its `OVE_*` substrate constant — and `<ove/types.h>`
 * independently pins every `OVE_ERR_*` to a literal integer.  A
 * renumbering in either layer breaks the build before producing a
 * binary.
 */

#pragma once

#include <ove/ove.h>

#include <expected>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ove
{

/**
 * @brief Strong-typed mirror of substrate `OVE_ERR_*` codes.
 *
 * Underlying type is `int` so `static_cast<int>(Error::Timeout)` gives
 * back the original substrate rc-value bit-for-bit.  Every variant is
 * pinned to its substrate constant via `static_assert` immediately
 * below this enum — a drift in either layer triggers a compile error.
 *
 * `Error::Ok` is included so the enum can round-trip a "no error"
 * value through `std::error_code` (where `value() == 0` denotes
 * success).  Constructing a `Result<T>` with `Error::Ok` as the error
 * state is a misuse — prefer @ref from_rc which maps `OVE_OK` to the
 * expected-value side of the `std::expected`.
 */
enum class Error : int {
	Ok = OVE_OK,
	NotRegistered = OVE_ERR_NOT_REGISTERED,
	InvalidParam = OVE_ERR_INVALID_PARAM,
	NoMemory = OVE_ERR_NO_MEMORY,
	Timeout = OVE_ERR_TIMEOUT,
	NotSupported = OVE_ERR_NOT_SUPPORTED,
	QueueFull = OVE_ERR_QUEUE_FULL,
	MlFailed = OVE_ERR_ML_FAILED,
	NetRefused = OVE_ERR_NET_REFUSED,
	NetUnreachable = OVE_ERR_NET_UNREACHABLE,
	NetAddrInUse = OVE_ERR_NET_ADDR_IN_USE,
	NetAddrNotAvailable = OVE_ERR_NET_ADDR_NOT_AVAILABLE,
	NetReset = OVE_ERR_NET_RESET,
	NetDnsFail = OVE_ERR_NET_DNS_FAIL,
	NetClosed = OVE_ERR_NET_CLOSED,
	BusNack = OVE_ERR_BUS_NACK,
	BusBusy = OVE_ERR_BUS_BUSY,
	BusError = OVE_ERR_BUS_ERROR,
	QueueEmpty = OVE_ERR_QUEUE_EMPTY,
	WouldBlock = OVE_ERR_WOULD_BLOCK,
	Eof = OVE_ERR_EOF,
	Inval = OVE_ERR_INVAL,
	NotFound = OVE_ERR_NOT_FOUND,
	AlreadyExists = OVE_ERR_ALREADY_EXISTS,
	NoSpace = OVE_ERR_NO_SPACE,
	NotDir = OVE_ERR_NOT_DIR,
	IsDir = OVE_ERR_IS_DIR,
	NotEmpty = OVE_ERR_NOT_EMPTY,
	ReadOnly = OVE_ERR_READ_ONLY,
	Io = OVE_ERR_IO,
	Busy = OVE_ERR_BUSY,
	NameTooLong = OVE_ERR_NAME_TOO_LONG,
	BadHandle = OVE_ERR_BAD_HANDLE,
	Permission = OVE_ERR_PERMISSION,
	CrossDevice = OVE_ERR_CROSS_DEVICE,
};

/*
 * Drift pin.  `<ove/types.h>` already pins every `OVE_ERR_*` to a
 * literal integer with `OVE_STATIC_ASSERT`; this second pass pins
 * `Error::*` to those same substrate values.  A future variant must
 * either land here too or have its substrate constant renumbered —
 * the silent-drift surface is closed at both ends.
 */
static_assert(static_cast<int>(Error::Ok) == OVE_OK, "Error::Ok drifted");
static_assert(static_cast<int>(Error::NotRegistered) == OVE_ERR_NOT_REGISTERED,
	      "Error::NotRegistered drifted");
static_assert(static_cast<int>(Error::InvalidParam) == OVE_ERR_INVALID_PARAM,
	      "Error::InvalidParam drifted");
static_assert(static_cast<int>(Error::NoMemory) == OVE_ERR_NO_MEMORY, "Error::NoMemory drifted");
static_assert(static_cast<int>(Error::Timeout) == OVE_ERR_TIMEOUT, "Error::Timeout drifted");
static_assert(static_cast<int>(Error::NotSupported) == OVE_ERR_NOT_SUPPORTED,
	      "Error::NotSupported drifted");
static_assert(static_cast<int>(Error::QueueFull) == OVE_ERR_QUEUE_FULL, "Error::QueueFull drifted");
static_assert(static_cast<int>(Error::MlFailed) == OVE_ERR_ML_FAILED, "Error::MlFailed drifted");
static_assert(static_cast<int>(Error::NetRefused) == OVE_ERR_NET_REFUSED,
	      "Error::NetRefused drifted");
static_assert(static_cast<int>(Error::NetUnreachable) == OVE_ERR_NET_UNREACHABLE,
	      "Error::NetUnreachable drifted");
static_assert(static_cast<int>(Error::NetAddrInUse) == OVE_ERR_NET_ADDR_IN_USE,
	      "Error::NetAddrInUse drifted");
static_assert(static_cast<int>(Error::NetAddrNotAvailable) == OVE_ERR_NET_ADDR_NOT_AVAILABLE,
	      "Error::NetAddrNotAvailable drifted");
static_assert(static_cast<int>(Error::NetReset) == OVE_ERR_NET_RESET, "Error::NetReset drifted");
static_assert(static_cast<int>(Error::NetDnsFail) == OVE_ERR_NET_DNS_FAIL,
	      "Error::NetDnsFail drifted");
static_assert(static_cast<int>(Error::NetClosed) == OVE_ERR_NET_CLOSED, "Error::NetClosed drifted");
static_assert(static_cast<int>(Error::BusNack) == OVE_ERR_BUS_NACK, "Error::BusNack drifted");
static_assert(static_cast<int>(Error::BusBusy) == OVE_ERR_BUS_BUSY, "Error::BusBusy drifted");
static_assert(static_cast<int>(Error::BusError) == OVE_ERR_BUS_ERROR, "Error::BusError drifted");
static_assert(static_cast<int>(Error::QueueEmpty) == OVE_ERR_QUEUE_EMPTY,
	      "Error::QueueEmpty drifted");
static_assert(static_cast<int>(Error::WouldBlock) == OVE_ERR_WOULD_BLOCK,
	      "Error::WouldBlock drifted");
static_assert(static_cast<int>(Error::Eof) == OVE_ERR_EOF, "Error::Eof drifted");
static_assert(static_cast<int>(Error::Inval) == OVE_ERR_INVAL, "Error::Inval drifted");
static_assert(static_cast<int>(Error::NotFound) == OVE_ERR_NOT_FOUND, "Error::NotFound drifted");
static_assert(static_cast<int>(Error::AlreadyExists) == OVE_ERR_ALREADY_EXISTS,
	      "Error::AlreadyExists drifted");
static_assert(static_cast<int>(Error::NoSpace) == OVE_ERR_NO_SPACE, "Error::NoSpace drifted");
static_assert(static_cast<int>(Error::NotDir) == OVE_ERR_NOT_DIR, "Error::NotDir drifted");
static_assert(static_cast<int>(Error::IsDir) == OVE_ERR_IS_DIR, "Error::IsDir drifted");
static_assert(static_cast<int>(Error::NotEmpty) == OVE_ERR_NOT_EMPTY, "Error::NotEmpty drifted");
static_assert(static_cast<int>(Error::ReadOnly) == OVE_ERR_READ_ONLY, "Error::ReadOnly drifted");
static_assert(static_cast<int>(Error::Io) == OVE_ERR_IO, "Error::Io drifted");
static_assert(static_cast<int>(Error::Busy) == OVE_ERR_BUSY, "Error::Busy drifted");
static_assert(static_cast<int>(Error::NameTooLong) == OVE_ERR_NAME_TOO_LONG,
	      "Error::NameTooLong drifted");
static_assert(static_cast<int>(Error::BadHandle) == OVE_ERR_BAD_HANDLE, "Error::BadHandle drifted");
static_assert(static_cast<int>(Error::Permission) == OVE_ERR_PERMISSION,
	      "Error::Permission drifted");
static_assert(static_cast<int>(Error::CrossDevice) == OVE_ERR_CROSS_DEVICE,
	      "Error::CrossDevice drifted");

/**
 * @brief `std::expected`-based result alias.
 *
 * Carries either a value of type @c T or an @ref Error.  Default
 * template argument `T = void` covers fallible operations that have
 * no success-side payload (`Result<void>` is the analogue of an
 * `int rc` with `OVE_OK` success semantics).
 *
 * Composes with the full `std::expected` monadic surface:
 * `.and_then`, `.or_else`, `.transform`, `.value_or`.
 */
template <class T = void> using Result = std::expected<T, Error>;

namespace detail
{

/**
 * @brief `std::error_category` implementation for @ref Error values.
 *
 * Kept inside `detail::` because user code should never construct
 * this directly — call @ref error_category() to obtain the singleton
 * reference instead.  The class is exposed only because
 * `std::error_category` requires inheritance.
 */
class error_category_impl : public std::error_category
{
      public:
	const char *name() const noexcept override
	{
		return "ove";
	}

	std::string message(int ev) const override
	{
		switch (static_cast<Error>(ev)) {
		case Error::Ok:
			return "success";
		case Error::NotRegistered:
			return "backend or feature not registered";
		case Error::InvalidParam:
			return "invalid parameter";
		case Error::NoMemory:
			return "out of memory";
		case Error::Timeout:
			return "operation timed out";
		case Error::NotSupported:
			return "operation not supported";
		case Error::QueueFull:
			return "queue full";
		case Error::MlFailed:
			return "ML inference failed";
		case Error::NetRefused:
			return "connection refused";
		case Error::NetUnreachable:
			return "network unreachable";
		case Error::NetAddrInUse:
			return "address in use";
		case Error::NetAddrNotAvailable:
			return "address not available";
		case Error::NetReset:
			return "connection reset by peer";
		case Error::NetDnsFail:
			return "DNS resolution failed";
		case Error::NetClosed:
			return "connection closed";
		case Error::BusNack:
			return "bus NACK";
		case Error::BusBusy:
			return "bus busy";
		case Error::BusError:
			return "bus framing/parity error";
		case Error::QueueEmpty:
			return "queue empty";
		case Error::WouldBlock:
			return "operation would block";
		case Error::Eof:
			return "end of file";
		case Error::Inval:
			return "argument or state invalid";
		case Error::NotFound:
			return "resource not found";
		case Error::AlreadyExists:
			return "filesystem entry already exists";
		case Error::NoSpace:
			return "no space left on device";
		case Error::NotDir:
			return "not a directory";
		case Error::IsDir:
			return "is a directory";
		case Error::NotEmpty:
			return "directory not empty";
		case Error::ReadOnly:
			return "read-only filesystem";
		case Error::Io:
			return "storage I/O error";
		case Error::Busy:
			return "resource busy";
		case Error::NameTooLong:
			return "name too long";
		case Error::BadHandle:
			return "bad handle";
		case Error::Permission:
			return "permission denied";
		case Error::CrossDevice:
			return "cross-device operation";
		}
		return "unknown ove error";
	}
};

} /* namespace detail */

/**
 * @brief Returns the singleton @c std::error_category for @ref Error.
 *
 * Thread-safe under C++11+ guaranteed initialisation of function-local
 * statics.  The returned reference has static storage duration.
 *
 * @return Reference to the canonical oveRTOS error category.
 */
inline const std::error_category &error_category() noexcept
{
	static const detail::error_category_impl instance;
	return instance;
}

/**
 * @brief ADL-discoverable factory for @c std::error_code from @ref Error.
 *
 * Together with the @c std::is_error_code_enum specialisation at the
 * bottom of this header, this enables implicit conversion:
 * @code
 *   std::error_code ec = ove::Error::Timeout;
 *   if (ec) std::cerr << ec.message() << '\n';
 * @endcode
 */
inline std::error_code make_error_code(Error e) noexcept
{
	return {static_cast<int>(e), error_category()};
}

/**
 * @brief Lifts a substrate rc-code into a `Result<void>`.
 *
 * Maps @c OVE_OK → expected value, anything else → @ref Error of the
 * corresponding variant.  This is the canonical bridge between the C
 * substrate's `int rc` convention and the C++ `Result<>` convention.
 *
 * @param rc Return code from a substrate call (e.g. `ove_mutex_lock`).
 * @return Empty `Result<void>` on success, `unexpected(Error)` on
 *         failure.
 */
[[nodiscard]] inline Result<void> from_rc(int rc) noexcept
{
	if (rc == OVE_OK)
		return {};
	return std::unexpected{static_cast<Error>(rc)};
}

/**
 * @brief Lifts a substrate rc + success-side value into a `Result<T>`.
 *
 * `from_rc(rc, value)` returns:
 *   - `Result<T>{std::forward<T>(value)}` if @p rc is @c OVE_OK
 *   - `std::unexpected{static_cast<Error>(rc)}` otherwise
 *
 * Useful for substrate calls that return both an rc and an out-value
 * — wrap the rc + value in a single expression on the way out.
 *
 * @tparam T Deduced from @p value; the success-side payload type
 *           after `std::decay` (so a string literal lands as
 *           `Result<const char*>`, not `Result<char[N]>`).
 * @param rc Return code from the substrate call.
 * @param value The success-side value, forwarded into the `Result`.
 */
template <class T> [[nodiscard]] inline Result<std::decay_t<T>> from_rc(int rc, T &&value)
{
	using V = std::decay_t<T>;
	if (rc == OVE_OK)
		return Result<V>{std::forward<T>(value)};
	return std::unexpected{static_cast<Error>(rc)};
}

} /* namespace ove */

/*
 * Enable `std::error_code ec = ove::Error::X;` — the standard lookup
 * rule is: if `is_error_code_enum<E>` is true, error_code's
 * converting constructor calls `make_error_code(E)` via ADL.
 */
namespace std
{
template <> struct is_error_code_enum<ove::Error> : true_type {
};
} /* namespace std */
