/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_TEST_CPP_HPP
#define OVE_TEST_CPP_HPP

/*
 * ⚠ cmocka + C++ RAII: cmocka's assert_*() macros report failure via
 * longjmp() (see <setjmp.h> below).  A longjmp does NOT unwind the C++ stack,
 * so any stack RAII object live at the point of a *failing* assertion has its
 * destructor SKIPPED — e.g. a `ove::LockGuard`/`ove::Mutex` is left
 * locked/leaked, and a destructor-based cleanup in the test body never runs.
 *
 * Impact is failure-path only (the test has already failed), so it does not
 * cause false passes, but it can muddy leak reports and, if a *shared* object
 * is left in a bad state, perturb a later test.  Guidance: keep asserts out of
 * scopes that hold RAII cleanup you depend on, or move cleanup into a cmocka
 * teardown (which DOES run after a failed assert).  Do not rely on stack
 * destructors for correctness across an assertion.
 */

/* Pull in <chrono> (via ove.hpp) BEFORE cmocka so std::chrono internals
 * don't collide with cmocka's bare `fail()` macro. */
#include "ove/ove.hpp"

extern "C" {
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
}

#include <atomic>
#include <type_traits>

#include "../../framework/ove_test_common.h"

static inline void test_msleep(uint32_t ms)
{
	ove::this_thread::sleep_ms(ms);
}

template <size_t StackSize = 4096>
inline ove::Thread<StackSize> make_test_thread(const char *name, ove_thread_fn entry,
					       void *arg = nullptr,
					       ove_prio_t prio = OVE_PRIO_NORMAL)
{
	return ove::Thread<StackSize>(entry, arg, prio, name);
}

/* Assert that an ove::Result<…> succeeded.  On failure prints the strong
 * Error code, then fails the test — so a submit/reset/send error reads
 * clearly instead of surfacing later as a misleading behavioural mismatch.
 * Mirrors the C-side OVE_TEST_ASSERT_OK in tests/framework/ove_test.h.
 * (Construction success is a separate idiom: assert_true(obj.valid()).) */
template <class T>
inline void ove_test_assert_ok(const ove::Result<T> &r, const char *expr, const char *file,
			       int line)
{
	if (!r.has_value()) {
		print_message("%s:%d: %s failed: Error(%d)\n", file, line, expr,
			      static_cast<int>(r.error()));
		_fail(file, line);
	}
}

#define OVE_TEST_ASSERT_OK(r) ove_test_assert_ok((r), #r, __FILE__, __LINE__)
#define assert_ok(r) OVE_TEST_ASSERT_OK(r)

/* C++ test suite runner declarations — single-sourced from suites.inc. */
#define OVE_CPP_SUITE(name, label) int test_cpp_##name##_run(void);
#include "suites.inc"
#undef OVE_CPP_SUITE

#endif /* OVE_TEST_CPP_HPP */
