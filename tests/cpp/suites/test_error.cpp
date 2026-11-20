/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Tests for ove::Error, Result<T>, error_category() and the
 * from_rc(...) lifters in <ove/error.hpp>.
 *
 * Scope: foundation layer.  Primitives migrate to Result<T> in their
 * own iterations — currently only ove::Mutex::try_lock_for/until.
 * These tests pin the public surface so per-primitive migrations can
 * rely on it.
 */

#include "../framework/ove_test.hpp"

#include <ove/error.hpp>

#include <string>
#include <type_traits>
#include <utility>

/* ── Error <-> int round-trip ────────────────────────────────────── */

static void test_cpp_error_int_roundtrip(void **state)
{
	(void)state;
	using ove::Error;
	/* Check every C++ variant against its C return code. */
	constexpr struct {
		Error error;
		int code;
	} known[] = {
		{Error::Ok, OVE_OK},
		{Error::NotRegistered, OVE_ERR_NOT_REGISTERED},
		{Error::InvalidParam, OVE_ERR_INVALID_PARAM},
		{Error::NoMemory, OVE_ERR_NO_MEMORY},
		{Error::Timeout, OVE_ERR_TIMEOUT},
		{Error::NotSupported, OVE_ERR_NOT_SUPPORTED},
		{Error::QueueFull, OVE_ERR_QUEUE_FULL},
		{Error::MlFailed, OVE_ERR_ML_FAILED},
		{Error::NetRefused, OVE_ERR_NET_REFUSED},
		{Error::NetUnreachable, OVE_ERR_NET_UNREACHABLE},
		{Error::NetAddrInUse, OVE_ERR_NET_ADDR_IN_USE},
		{Error::NetAddrNotAvailable, OVE_ERR_NET_ADDR_NOT_AVAILABLE},
		{Error::NetReset, OVE_ERR_NET_RESET},
		{Error::NetDnsFail, OVE_ERR_NET_DNS_FAIL},
		{Error::NetClosed, OVE_ERR_NET_CLOSED},
		{Error::BusNack, OVE_ERR_BUS_NACK},
		{Error::BusBusy, OVE_ERR_BUS_BUSY},
		{Error::BusError, OVE_ERR_BUS_ERROR},
		{Error::QueueEmpty, OVE_ERR_QUEUE_EMPTY},
		{Error::WouldBlock, OVE_ERR_WOULD_BLOCK},
		{Error::Eof, OVE_ERR_EOF},
		{Error::Inval, OVE_ERR_INVAL},
		{Error::NotFound, OVE_ERR_NOT_FOUND},
		{Error::AlreadyExists, OVE_ERR_ALREADY_EXISTS},
		{Error::NoSpace, OVE_ERR_NO_SPACE},
		{Error::NotDir, OVE_ERR_NOT_DIR},
		{Error::IsDir, OVE_ERR_IS_DIR},
		{Error::NotEmpty, OVE_ERR_NOT_EMPTY},
		{Error::ReadOnly, OVE_ERR_READ_ONLY},
		{Error::Io, OVE_ERR_IO},
		{Error::Busy, OVE_ERR_BUSY},
		{Error::NameTooLong, OVE_ERR_NAME_TOO_LONG},
		{Error::BadHandle, OVE_ERR_BAD_HANDLE},
		{Error::Permission, OVE_ERR_PERMISSION},
		{Error::CrossDevice, OVE_ERR_CROSS_DEVICE},
	};
	for (const auto &[error, code] : known) {
		assert_int_equal(static_cast<int>(error), code);
		const auto result = ove::from_rc(code);
		if (code == OVE_OK) {
			assert_true(result.has_value());
		} else {
			assert_false(result.has_value());
			assert_true(result.error() == error);
		}
		const auto msg = ove::make_error_code(error).message();
		assert_false(msg.empty());
		assert_true(msg.find("unknown") == std::string::npos);
	}
}

/* ── from_rc(int) -> Result<void> ───────────────────────────────── */

static void test_cpp_error_from_rc_void_ok(void **state)
{
	(void)state;
	ove::Result<void> r = ove::from_rc(OVE_OK);
	assert_true(r.has_value());
}

static void test_cpp_error_from_rc_void_err(void **state)
{
	(void)state;
	ove::Result<void> r = ove::from_rc(OVE_ERR_TIMEOUT);
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout);
}

/* ── from_rc(int, T&&) -> Result<T> ─────────────────────────────── */

static void test_cpp_error_from_rc_value_ok(void **state)
{
	(void)state;
	ove::Result<int> r = ove::from_rc(OVE_OK, 42);
	assert_true(r.has_value());
	assert_int_equal(*r, 42);
}

static void test_cpp_error_from_rc_value_err(void **state)
{
	(void)state;
	ove::Result<int> r = ove::from_rc(OVE_ERR_QUEUE_FULL, 42);
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::QueueFull);
}

/* Move-only payload survives from_rc forwarding. */
static void test_cpp_error_from_rc_move_only(void **state)
{
	(void)state;
	struct MoveOnly {
		int v;
		MoveOnly(int v_) : v(v_)
		{
		}
		MoveOnly(MoveOnly &&) noexcept = default;
		MoveOnly &operator=(MoveOnly &&) noexcept = default;
		MoveOnly(const MoveOnly &) = delete;
		MoveOnly &operator=(const MoveOnly &) = delete;
	};

	ove::Result<MoveOnly> r = ove::from_rc(OVE_OK, MoveOnly{7});
	assert_true(r.has_value());
	assert_int_equal(r->v, 7);
}

/* String-literal arg decays to const char* — confirms decay. */
static void test_cpp_error_from_rc_decay(void **state)
{
	(void)state;
	auto r = ove::from_rc(OVE_OK, "hello");
	static_assert(std::is_same_v<decltype(r), ove::Result<const char *>>,
		      "from_rc must decay array-to-pointer for the success type");
	assert_true(r.has_value());
	assert_string_equal(*r, "hello");
}

/* ── std::error_code interop ────────────────────────────────────── */

static void test_cpp_error_error_code_implicit(void **state)
{
	(void)state;
	std::error_code ec = ove::Error::Timeout;
	assert_int_equal(ec.value(), OVE_ERR_TIMEOUT);
	/* `ec.category() == error_category()` checks ADL-found category. */
	assert_true(ec.category() == ove::error_category());
	assert_string_equal(ec.category().name(), "ove");
}

static void test_cpp_error_error_code_message(void **state)
{
	(void)state;
	std::error_code ec = ove::make_error_code(ove::Error::QueueFull);
	std::string msg = ec.message();
	/* Don't pin exact wording — just confirm it's a non-empty,
	 * non-"unknown" answer.  Wording changes shouldn't break tests. */
	assert_true(!msg.empty());
	assert_true(msg.find("unknown") == std::string::npos);
}

static void test_cpp_error_error_code_ok_falsy(void **state)
{
	(void)state;
	/* `std::error_code` of value 0 is falsy regardless of category. */
	std::error_code ec = ove::Error::Ok;
	assert_false(static_cast<bool>(ec));
}

static void test_cpp_error_message_unknown_value(void **state)
{
	(void)state;
	/* A non-enum integer value still has to produce a string — the
	 * implementation falls through to "unknown ove error". */
	std::string msg = ove::error_category().message(0x7BAD);
	assert_string_equal(msg.c_str(), "unknown ove error");
}

/* ── Result<T> std::expected semantics ──────────────────────────── */

static void test_cpp_error_result_value_or(void **state)
{
	(void)state;
	ove::Result<int> ok = ove::from_rc(OVE_OK, 10);
	ove::Result<int> err = ove::from_rc(OVE_ERR_TIMEOUT, 10);
	assert_int_equal(ok.value_or(-1), 10);
	assert_int_equal(err.value_or(-1), -1);
}

static void test_cpp_error_result_and_then(void **state)
{
	(void)state;
	auto doubled =
		ove::from_rc(OVE_OK, 21).and_then([](int v) -> ove::Result<int> { return v * 2; });
	assert_true(doubled.has_value());
	assert_int_equal(*doubled, 42);

	auto short_circuit =
		ove::from_rc(OVE_ERR_TIMEOUT, 21).and_then([](int v) -> ove::Result<int> {
			return v * 2;
		});
	assert_false(short_circuit.has_value());
	assert_true(short_circuit.error() == ove::Error::Timeout);
}

static void test_cpp_error_result_or_else(void **state)
{
	(void)state;
	/* or_else maps the error path; success short-circuits. */
	auto recovered =
		ove::from_rc(OVE_ERR_TIMEOUT, 0).or_else([](ove::Error e) -> ove::Result<int> {
			(void)e;
			return 99;
		});
	assert_true(recovered.has_value());
	assert_int_equal(*recovered, 99);
}

/* ── Type-shape static_asserts ──────────────────────────────────── */

static void test_cpp_error_type_shape(void **state)
{
	(void)state;
	static_assert(std::is_same_v<ove::Result<>, ove::Result<void>>,
		      "Result<> default template arg must be void");
	static_assert(std::is_same_v<ove::Result<int>, std::expected<int, ove::Error>>,
		      "Result<T> must be std::expected<T, Error>");
	static_assert(std::is_enum_v<ove::Error>, "Error must be an enum");
	static_assert(std::is_same_v<std::underlying_type_t<ove::Error>, int>,
		      "Error underlying type must be int (matches substrate rc)");
	static_assert(std::is_error_code_enum_v<ove::Error>,
		      "Error must opt into std::error_code interop");
}

int test_cpp_error_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_error_int_roundtrip),
		cmocka_unit_test(test_cpp_error_from_rc_void_ok),
		cmocka_unit_test(test_cpp_error_from_rc_void_err),
		cmocka_unit_test(test_cpp_error_from_rc_value_ok),
		cmocka_unit_test(test_cpp_error_from_rc_value_err),
		cmocka_unit_test(test_cpp_error_from_rc_move_only),
		cmocka_unit_test(test_cpp_error_from_rc_decay),
		cmocka_unit_test(test_cpp_error_error_code_implicit),
		cmocka_unit_test(test_cpp_error_error_code_message),
		cmocka_unit_test(test_cpp_error_error_code_ok_falsy),
		cmocka_unit_test(test_cpp_error_message_unknown_value),
		cmocka_unit_test(test_cpp_error_result_value_or),
		cmocka_unit_test(test_cpp_error_result_and_then),
		cmocka_unit_test(test_cpp_error_result_or_else),
		cmocka_unit_test(test_cpp_error_type_shape),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
