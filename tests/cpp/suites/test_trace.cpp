/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Compile/behavior smoke test for the C++ trace wrappers. The host
 * configuration builds with trace support off, so these exercise the
 * no-op stubs; the wire format is covered by tests/obs/test_trace_api.c
 * and the dashboard parser tests.
 */

#include "../framework/ove_test.hpp"

#include <ove/trace.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_nothrow_destructible_v<ove::trace::Zone>);
static_assert(std::is_nothrow_move_constructible_v<ove::trace::Zone>);

static void test_cpp_trace_mark_and_name(void **state)
{
	(void)state;
	/* The host configuration builds with trace support off, so the
	 * registration is refused there; the call must still compile and
	 * report its outcome. */
	const bool registered = ove::trace::name(7, "draw");
	(void)registered;
	ove::trace::mark(7);
	ove::trace::zone_begin(7);
	ove::trace::zone_end(7);
}

static void test_cpp_trace_zone_raii(void **state)
{
	(void)state;
	{
		ove::trace::Zone z(9);
		ove::trace::Zone moved(std::move(z));
		(void)moved;
	}
	/* A moved-from Zone must not close the id out from under its owner. */
	ove::trace::Zone outer(10);
	ove::trace::Zone inner(std::move(outer));
	(void)inner;
}

int test_cpp_trace_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_trace_mark_and_name),
		cmocka_unit_test(test_cpp_trace_zone_raii),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
