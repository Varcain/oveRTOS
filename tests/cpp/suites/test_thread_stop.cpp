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
#include <chrono>
#include <map>
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

/* ── 6. join() blocks until the cooperative worker exits ──────────────
 *      Iter 6a: explicit join — std::thread::join analog.  After join
 *      the wrapper is empty; the destructor at end of scope is a
 *      no-op. */

static void test_join_returns_after_worker_exits(void **state)
{
	(void)state;
	reset_flags();
	ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "joincoop"};
	bool seen = false;
	for (int i = 0; i < 200 && !seen; ++i) {
		if (g_observed_false_before_request.load())
			seen = true;
		else
			test_msleep(1);
	}
	assert_true(seen);

	th.request_stop();
	th.join();

	/* After join: worker has fully exited, wrapper is empty. */
	assert_int_equal(g_observed_true_after_request.load(), 1);
	assert_int_equal(g_exited.load(), 1);
	assert_false(th.valid());
	/* Destructor at end of scope is a no-op (handle_ is null). */
}

/* ── 7. join() leaves destructor as no-op (no double-destroy) ───────── */

static void test_join_makes_destructor_noop(void **state)
{
	(void)state;
	reset_flags();
	{
		ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "j2"};
		for (int i = 0; i < 200 && !g_observed_false_before_request.load(); ++i)
			test_msleep(1);
		th.request_stop();
		th.join();
		/* Destructor fires at end of scope.  If join didn't null
		 * the handle, this would double-call destroy/deinit and
		 * trip an assertion or UAF in the substrate. */
	}
	assert_int_equal(g_exited.load(), 1);
}

/* ── 8. detach() skips the destructor's join wait ─────────────────────
 *      Worker self-terminates on an external flag, NOT stop_token.
 *      Without detach, the destructor would block waiting for the
 *      worker to exit, which doesn't happen until we set the flag. */

static std::atomic<int> g_detach_keep_running{1};
static std::atomic<int> g_detach_exited{0};

static void detach_worker(ove::stop_token /*tok*/)
{
	while (g_detach_keep_running.load())
		test_msleep(2);
	g_detach_exited.store(1);
}

static void test_detach_skips_join(void **state)
{
	(void)state;
	g_detach_keep_running.store(1);
	g_detach_exited.store(0);

	const auto t_start = std::chrono::steady_clock::now();
	{
		ove::Thread<4096> th{detach_worker, OVE_PRIO_NORMAL, "det"};
		th.detach();
		/* Destructor at end of scope MUST NOT block — handle is
		 * already null. */
	}
	const auto t_elapsed = std::chrono::steady_clock::now() - t_start;
	const auto ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(t_elapsed).count();
	assert_true(ms < 100); /* would block ≥ many seconds without detach */

	/* Clean up: tell the worker to exit, wait for it. */
	g_detach_keep_running.store(0);
	for (int i = 0; i < 500 && !g_detach_exited.load(); ++i)
		test_msleep(1);
	assert_int_equal(g_detach_exited.load(), 1);
}

/* ── 9. detach() does NOT request_stop on the running thread ─────────
 *      Verified by capturing a stop_token before detach and confirming
 *      the flag stays clear after detach.  The destructor sets the
 *      flag (jthread-style); detach must not. */

static void test_detach_does_not_signal_stop(void **state)
{
	(void)state;
	g_detach_keep_running.store(1);
	g_detach_exited.store(0);

	ove::stop_token captured;
	{
		ove::Thread<4096> th{detach_worker, OVE_PRIO_NORMAL, "det2"};
		captured = th.get_stop_token();
		assert_true(captured.stop_possible());
		assert_false(captured.stop_requested());
		th.detach();
	}
	/* After detach, the stop flag is still clear — detach is NOT
	 * request_stop. */
	assert_false(captured.stop_requested());

	/* Clean up. */
	g_detach_keep_running.store(0);
	for (int i = 0; i < 500 && !g_detach_exited.load(); ++i)
		test_msleep(1);
	assert_int_equal(g_detach_exited.load(), 1);
}

/* ── 10. joinable() tracks valid() across the wrapper's lifecycle ──── */

static void test_joinable_matches_valid(void **state)
{
	(void)state;
	reset_flags();
	{
		ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "jn"};
		assert_true(th.joinable());
		assert_true(th.valid());

		th.request_stop();
		th.join();

		assert_false(th.joinable());
		assert_false(th.valid());
	}
}

/* ── 11. get_id() returns a non-default id for a live thread ────────── */

static void test_get_id_non_default_for_live_thread(void **state)
{
	(void)state;
	reset_flags();
	ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "id1"};
	const ove::Thread<>::id tid = th.get_id();
	assert_true(tid != ove::thread_id{});
	assert_non_null(tid.native_handle());

	th.request_stop();
	th.join();
	/* After join: get_id() reflects the now-empty wrapper. */
	assert_true(th.get_id() == ove::thread_id{});
}

/* ── 12. id equality is stable for the same thread ──────────────────── */

static void test_id_equality_same_thread(void **state)
{
	(void)state;
	reset_flags();
	ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "id2"};
	const auto a = th.get_id();
	const auto b = th.get_id();
	assert_true(a == b);
	th.request_stop();
}

/* ── 13. default-constructed id distinct from any live id ───────────── */

static void test_id_default_ctor_distinct(void **state)
{
	(void)state;
	reset_flags();
	const ove::thread_id none{};
	ove::Thread<4096> th{cooperative_worker, OVE_PRIO_NORMAL, "id3"};
	assert_true(th.get_id() != none);
	th.request_stop();
}

/* ── 14. id is usable as a std::map key (ordering works) ─────────────
 *      Two cooperative threads -> two distinct ids -> two map entries.
 *      Heap-mode-only because std::map needs an allocator. */

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_id_ordering_works_in_map(void **state)
{
	(void)state;
	reset_flags();
	ove::Thread<4096> th_a{cooperative_worker, OVE_PRIO_NORMAL, "ma"};
	ove::Thread<4096> th_b{cooperative_worker, OVE_PRIO_NORMAL, "mb"};

	std::map<ove::Thread<>::id, const char *> names;
	names[th_a.get_id()] = "a";
	names[th_b.get_id()] = "b";

	assert_int_equal(names.size(), 2);
	assert_string_equal(names[th_a.get_id()], "a");
	assert_string_equal(names[th_b.get_id()], "b");

	th_a.request_stop();
	th_b.request_stop();
}
#endif

/* ── runner ─────────────────────────────────────────────────────────── */

int test_cpp_thread_stop_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cooperative_drop_exits_cleanly),
		cmocka_unit_test(test_explicit_request_stop),
		cmocka_unit_test(test_get_stop_token_shareable),
		cmocka_unit_test(test_legacy_entry_unaffected),
		cmocka_unit_test(test_default_stop_token_is_empty),
		cmocka_unit_test(test_join_returns_after_worker_exits),
		cmocka_unit_test(test_join_makes_destructor_noop),
		cmocka_unit_test(test_detach_skips_join),
		cmocka_unit_test(test_detach_does_not_signal_stop),
		cmocka_unit_test(test_joinable_matches_valid),
		cmocka_unit_test(test_get_id_non_default_for_live_thread),
		cmocka_unit_test(test_id_equality_same_thread),
		cmocka_unit_test(test_id_default_ctor_distinct),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_id_ordering_works_in_map),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
