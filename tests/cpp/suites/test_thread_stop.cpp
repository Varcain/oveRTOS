/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * C++ binding integration of cooperative cancellation (Phase 4 / Iteration 3):
 *   - ove::stop_token receives the per-thread cancellation flag.
 *   - The new Thread(F, prio, name) constructor accepts a
 *     `void(stop_token)` entry — std::jthread analog.
 *   - Thread::~Thread() calls request_stop() before the join wait so
 *     cooperative workers exit cleanly without deadlocking.
 *   - The legacy `void(void*)` constructor is unchanged.
 */

#include "../framework/ove_test.hpp"

#include <atomic>
#include <memory>

/* Per-test shared state.  std::atomic so the asserts after thread
 * destruction see fully-published values without memory-order surprises. */
static std::atomic<int> g_observed_false_before_request{0};
static std::atomic<int> g_observed_true_after_request{0};
static std::atomic<int> g_exited{0};

static void reset_flags()
{
	g_observed_false_before_request.store(0);
	g_observed_true_after_request.store(0);
	g_exited.store(0);
}

/* ── 1. cooperative worker exits when Thread goes out of scope ──────── */

static void cooperative_worker(ove::stop_token tok)
{
	if (!tok.stop_requested())
		g_observed_false_before_request.store(1);
	while (!tok.stop_requested())
		test_msleep(2);
	g_observed_true_after_request.store(1);
	g_exited.store(1);
}

static void test_cooperative_drop_exits_cleanly(void **state)
{
	(void)state;
	reset_flags();
	{
		ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "coop"};
		/* Worker observes false on entry within a few ms. */
		bool seen = false;
		for (int i = 0; i < 200 && !seen; ++i) {
			if (g_observed_false_before_request.load())
				seen = true;
			else
				test_msleep(1);
		}
		assert_true(seen);
		/* th goes out of scope here — ~Thread calls request_stop()
		 * before the join wait.  Worker sees the flag and exits. */
	}
	/* Worker must have observed true and exited cleanly. */
	assert_int_equal(g_observed_true_after_request.load(), 1);
	assert_int_equal(g_exited.load(), 1);
}

/* ── 2. explicit request_stop() before destruction ──────────────────── */

static void test_explicit_request_stop(void **state)
{
	(void)state;
	reset_flags();
	{
		ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "coop"};
		bool seen = false;
		for (int i = 0; i < 200 && !seen; ++i) {
			if (g_observed_false_before_request.load())
				seen = true;
			else
				test_msleep(1);
		}
		assert_true(seen);
		assert_false(th.stop_requested());
		th.request_stop();
		assert_true(th.stop_requested());
	}
	assert_int_equal(g_exited.load(), 1);
}

/* ── 3. get_stop_token can be passed to helper functions ────────────── */

static bool helper_checks_token(ove::stop_token tok)
{
	return tok.stop_possible() && tok.stop_requested();
}

static void test_get_stop_token_shareable(void **state)
{
	(void)state;
	reset_flags();
	ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "coop"};
	auto tok = th.get_stop_token();
	assert_true(tok.stop_possible());
	assert_false(helper_checks_token(tok));
	th.request_stop();
	assert_true(helper_checks_token(tok));
	/* destructor cleans up — worker already exited */
}

/* ── 4. legacy void(void*) entry still works (no token, no request_stop hook) ── */

static std::atomic<int> g_legacy_ran{0};

extern "C" void legacy_entry(void *arg)
{
	(void)arg;
	g_legacy_ran.store(1);
}

static void test_legacy_entry_unaffected(void **state)
{
	(void)state;
	g_legacy_ran.store(0);
	{
		ove::Thread<4096> th{legacy_entry, nullptr, OVE_PRIO_NORMAL, "legacy"};
		/* Worker runs once and returns.  Destructor will still call
		 * request_stop (harmless on a returned thread) and join. */
	}
	assert_int_equal(g_legacy_ran.load(), 1);
}

/* ── 5. default-constructed stop_token: stop_possible() is false ────── */

static void test_default_stop_token_is_empty(void **state)
{
	(void)state;
	ove::stop_token tok;
	assert_false(tok.stop_possible());
	assert_false(tok.stop_requested());
}

/* ── runner ─────────────────────────────────────────────────────────── */

int test_cpp_thread_stop_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cooperative_drop_exits_cleanly),
		cmocka_unit_test(test_explicit_request_stop),
		cmocka_unit_test(test_get_stop_token_shareable),
		cmocka_unit_test(test_legacy_entry_unaffected),
		cmocka_unit_test(test_default_stop_token_is_empty),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
