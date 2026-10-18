/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.hpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * ove::StaticCell lifecycle contract:
 *  - the cell reports "initialised" only once T's constructor has finished
 *    (get()/is_initialized() must never expose a half-built object);
 *  - the contained object's lifetime ends with the cell (~T runs);
 *  - a second init() never constructs over the live object; in a release
 *    (NDEBUG) build, where OVE_STATIC_INIT_ASSERT checks nothing, it traps.
 */

/* Implemented in static_cell_ndebug.cpp, which is compiled with NDEBUG. */
extern void static_cell_ndebug_double_init(int ctor_fd);

using namespace std::chrono_literals;

/* ── Publication happens only after construction ────────────────────── */

namespace
{

struct SlowCtor {
	int a;
	SlowCtor(ove::Event &in_ctor, ove::Event &go)
	{
		a = 1;
		in_ctor.signal();
		/* Bounded so a broken test can never wedge the runner. */
		(void)go.try_wait_for(2s);
		a = 2;
	}
};

struct SlowCtx {
	ove::StaticCell<SlowCtor> *cell;
	ove::Event *in_ctor;
	ove::Event *go;
	std::atomic<int> done{0};
};

} /* namespace */

extern "C" void static_cell_slow_init_entry(void *arg)
{
	auto *ctx = static_cast<SlowCtx *>(arg);
	ctx->cell->init(*ctx->in_ctor, *ctx->go);
	ctx->done.store(1);
}

static void test_cpp_static_cell_not_initialized_during_ctor(void **state)
{
	(void)state;
	ove::StaticCell<SlowCtor> cell;
	ove::Event in_ctor;
	ove::Event go;
	SlowCtx ctx{&cell, &in_ctor, &go};

	bool seen_during_ctor;
	bool ctor_entered;
	{
		ove::Thread<8192> t(static_cell_slow_init_entry, &ctx, OVE_PRIO_NORMAL, "sc_init");
		ctor_entered = in_ctor.try_wait_for(2s).has_value();
		/* Thread B is parked inside T's constructor right now. */
		seen_during_ctor = cell.is_initialized();
		go.signal();
		t.join();
	}
	/* Asserts only after the worker is joined (cmocka longjmps). */
	print_message("    is_initialized() while T() still running: %s (expect false)\n",
		      seen_during_ctor ? "true" : "false");
	assert_true(ctor_entered);
	assert_int_equal(ctx.done.load(), 1);
	assert_false(seen_during_ctor);
	assert_true(cell.is_initialized());
	assert_int_equal(cell.get().a, 2);
}

/* ── Contained object is destroyed with the cell ────────────────────── */

namespace
{

std::atomic<int> g_counted_ctors;
std::atomic<int> g_counted_dtors;

struct Counted {
	int v;
	explicit Counted(int x) : v(x)
	{
		g_counted_ctors.fetch_add(1);
	}
	~Counted()
	{
		g_counted_dtors.fetch_add(1);
	}
};

} /* namespace */

static void test_cpp_static_cell_dtor_runs(void **state)
{
	(void)state;
	g_counted_ctors.store(0);
	g_counted_dtors.store(0);
	{
		ove::StaticCell<Counted> cell;
		assert_int_equal(cell.init(7).v, 7);
	}
	print_message("    ctors=%d dtors=%d after cell left scope (expect 1/1)\n",
		      g_counted_ctors.load(), g_counted_dtors.load());
	assert_int_equal(g_counted_ctors.load(), 1);
	assert_int_equal(g_counted_dtors.load(), 1);
}

static void test_cpp_static_cell_uninit_dtor_noop(void **state)
{
	(void)state;
	g_counted_dtors.store(0);
	{
		ove::StaticCell<Counted> cell;
		assert_false(cell.is_initialized());
	}
	assert_int_equal(g_counted_dtors.load(), 0);
}

/* ── Release build: second init() traps instead of re-constructing ──── */

static void test_cpp_static_cell_double_init_ndebug(void **state)
{
	(void)state;
	int fds[2];
	assert_int_equal(pipe(fds), 0);
	const pid_t pid = fork();
	assert_true(pid >= 0);
	if (pid == 0) {
		/* cmocka's inherited handlers would catch the trap and carry on
		 * running the suite in the child: die on it instead. */
		for (int sig : {SIGILL, SIGTRAP, SIGABRT, SIGSEGV, SIGBUS})
			(void)signal(sig, SIG_DFL);
		const struct rlimit no_core = {0, 0};
		(void)setrlimit(RLIMIT_CORE, &no_core);
		close(fds[0]);
		static_cell_ndebug_double_init(fds[1]);
		_exit(0); /* reached only if the second init() returned */
	}
	close(fds[1]);
	char buf[8];
	ssize_t n;
	int ctors = 0;
	while ((n = read(fds[0], buf, sizeof(buf))) > 0)
		ctors += (int)n;
	close(fds[0]);
	int status = 0;
	assert_int_equal(waitpid(pid, &status, 0), pid);
	print_message("    NDEBUG: second init() %s after %d construction(s) (expect trap/1)\n",
		      WIFSIGNALED(status) ? "trapped" : "returned", ctors);
	assert_true(WIFSIGNALED(status));
	assert_int_equal(ctors, 1);
}

/* ── Suite runner ───────────────────────────────────────────────────── */

int test_cpp_static_cell_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_static_cell_not_initialized_during_ctor),
		cmocka_unit_test(test_cpp_static_cell_dtor_runs),
		cmocka_unit_test(test_cpp_static_cell_uninit_dtor_noop),
		cmocka_unit_test(test_cpp_static_cell_double_init_ndebug),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
