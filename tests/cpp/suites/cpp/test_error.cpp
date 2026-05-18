/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Tests for ove::cpp::Error, Result<T>, ove_category() and the
 * from_rc(...) lifters in <ove/cpp/error.hpp>.
 *
 * Scope: foundation layer only — no primitive uses Result<T> yet.
 * These tests pin the public surface so that subsequent ove::cpp::*
 * primitives can rely on it.
 */

#include "../../framework/ove_test.hpp"

#include <ove/cpp/error.hpp>

#include <string>
#include <type_traits>
#include <utility>

namespace cppns = ove::cpp;

/* ── Error <-> int round-trip ────────────────────────────────────── */

static void test_cppns_error_int_roundtrip(void **state)
{
	(void)state;
	/* Drift-pin sanity: cast both directions for representative
	 * variants.  The exhaustive static_assert block lives in
	 * <ove/cpp/error.hpp>; these are runtime checks just to confirm
	 * the build's `static_cast<int>(Error::X) == OVE_ERR_*` answer
	 * also holds at runtime under the same flags. */
	assert_int_equal(static_cast<int>(cppns::Error::Ok), OVE_OK);
	assert_int_equal(static_cast<int>(cppns::Error::Timeout), OVE_ERR_TIMEOUT);
	assert_int_equal(static_cast<int>(cppns::Error::QueueFull), OVE_ERR_QUEUE_FULL);
	assert_int_equal(static_cast<int>(cppns::Error::NotFound), OVE_ERR_NOT_FOUND);
}

/* ── from_rc(int) -> Result<void> ───────────────────────────────── */

static void test_cppns_error_from_rc_void_ok(void **state)
{
	(void)state;
	cppns::Result<void> r = cppns::from_rc(OVE_OK);
	assert_true(r.has_value());
}

static void test_cppns_error_from_rc_void_err(void **state)
{
	(void)state;
	cppns::Result<void> r = cppns::from_rc(OVE_ERR_TIMEOUT);
	assert_false(r.has_value());
	assert_true(r.error() == cppns::Error::Timeout);
}

/* ── from_rc(int, T&&) -> Result<T> ─────────────────────────────── */

static void test_cppns_error_from_rc_value_ok(void **state)
{
	(void)state;
	cppns::Result<int> r = cppns::from_rc(OVE_OK, 42);
	assert_true(r.has_value());
	assert_int_equal(*r, 42);
}

static void test_cppns_error_from_rc_value_err(void **state)
{
	(void)state;
	cppns::Result<int> r = cppns::from_rc(OVE_ERR_QUEUE_FULL, 42);
	assert_false(r.has_value());
	assert_true(r.error() == cppns::Error::QueueFull);
}

/* Move-only payload survives from_rc forwarding. */
static void test_cppns_error_from_rc_move_only(void **state)
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

	cppns::Result<MoveOnly> r = cppns::from_rc(OVE_OK, MoveOnly{7});
	assert_true(r.has_value());
	assert_int_equal(r->v, 7);
}

/* String-literal arg decays to const char* — confirms remove_cvref. */
static void test_cppns_error_from_rc_decay(void **state)
{
	(void)state;
	auto r = cppns::from_rc(OVE_OK, "hello");
	static_assert(std::is_same_v<decltype(r), cppns::Result<const char *>>,
		      "from_rc must decay array-to-pointer for the success type");
	assert_true(r.has_value());
	assert_string_equal(*r, "hello");
}

/* ── std::error_code interop ────────────────────────────────────── */

static void test_cppns_error_error_code_implicit(void **state)
{
	(void)state;
	std::error_code ec = cppns::Error::Timeout;
	assert_int_equal(ec.value(), OVE_ERR_TIMEOUT);
	/* `ec.category() == ove_category()` checks ADL-found category. */
	assert_true(ec.category() == cppns::ove_category());
	assert_string_equal(ec.category().name(), "ove");
}

static void test_cppns_error_error_code_message(void **state)
{
	(void)state;
	std::error_code ec = cppns::make_error_code(cppns::Error::QueueFull);
	std::string msg = ec.message();
	/* Don't pin exact wording — just confirm it's a non-empty,
	 * non-"unknown" answer.  Wording changes shouldn't break tests. */
	assert_true(!msg.empty());
	assert_true(msg.find("unknown") == std::string::npos);
}

static void test_cppns_error_error_code_ok_falsy(void **state)
{
	(void)state;
	/* `std::error_code` of value 0 is falsy regardless of category. */
	std::error_code ec = cppns::Error::Ok;
	assert_false(static_cast<bool>(ec));
}

static void test_cppns_error_message_unknown_value(void **state)
{
	(void)state;
	/* A non-enum integer value still has to produce a string — the
	 * implementation falls through to "unknown ove error". */
	std::string msg = cppns::ove_category().message(0x7BAD);
	assert_string_equal(msg.c_str(), "unknown ove error");
}

/* ── Result<T> std::expected semantics ──────────────────────────── */

static void test_cppns_error_result_value_or(void **state)
{
	(void)state;
	cppns::Result<int> ok = cppns::from_rc(OVE_OK, 10);
	cppns::Result<int> err = cppns::from_rc(OVE_ERR_TIMEOUT, 10);
	assert_int_equal(ok.value_or(-1), 10);
	assert_int_equal(err.value_or(-1), -1);
}

static void test_cppns_error_result_and_then(void **state)
{
	(void)state;
	auto doubled = cppns::from_rc(OVE_OK, 21).and_then([](int v) -> cppns::Result<int> {
		return v * 2;
	});
	assert_true(doubled.has_value());
	assert_int_equal(*doubled, 42);

	auto short_circuit = cppns::from_rc(OVE_ERR_TIMEOUT, 21)
				     .and_then([](int v) -> cppns::Result<int> { return v * 2; });
	assert_false(short_circuit.has_value());
	assert_true(short_circuit.error() == cppns::Error::Timeout);
}

static void test_cppns_error_result_or_else(void **state)
{
	(void)state;
	/* or_else maps the error path; success short-circuits. */
	auto recovered =
		cppns::from_rc(OVE_ERR_TIMEOUT, 0).or_else([](cppns::Error e) -> cppns::Result<int> {
			(void)e;
			return 99;
		});
	assert_true(recovered.has_value());
	assert_int_equal(*recovered, 99);
}

/* ── Type-shape static_asserts ──────────────────────────────────── */

static void test_cppns_error_type_shape(void **state)
{
	(void)state;
	static_assert(std::is_same_v<cppns::Result<>, cppns::Result<void>>,
		      "Result<> default template arg must be void");
	static_assert(std::is_same_v<cppns::Result<int>, std::expected<int, cppns::Error>>,
		      "Result<T> must be std::expected<T, Error>");
	static_assert(std::is_enum_v<cppns::Error>, "Error must be an enum");
	static_assert(std::is_same_v<std::underlying_type_t<cppns::Error>, int>,
		      "Error underlying type must be int (matches substrate rc)");
	static_assert(std::is_error_code_enum_v<cppns::Error>,
		      "Error must opt into std::error_code interop");
}

int test_cppns_error_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cppns_error_int_roundtrip),
		cmocka_unit_test(test_cppns_error_from_rc_void_ok),
		cmocka_unit_test(test_cppns_error_from_rc_void_err),
		cmocka_unit_test(test_cppns_error_from_rc_value_ok),
		cmocka_unit_test(test_cppns_error_from_rc_value_err),
		cmocka_unit_test(test_cppns_error_from_rc_move_only),
		cmocka_unit_test(test_cppns_error_from_rc_decay),
		cmocka_unit_test(test_cppns_error_error_code_implicit),
		cmocka_unit_test(test_cppns_error_error_code_message),
		cmocka_unit_test(test_cppns_error_error_code_ok_falsy),
		cmocka_unit_test(test_cppns_error_message_unknown_value),
		cmocka_unit_test(test_cppns_error_result_value_or),
		cmocka_unit_test(test_cppns_error_result_and_then),
		cmocka_unit_test(test_cppns_error_result_or_else),
		cmocka_unit_test(test_cppns_error_type_shape),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
