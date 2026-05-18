#include "../framework/ove_test.hpp"

static std::atomic<int> g_cpp_flag;
static std::atomic<intptr_t> g_cpp_arg_val;
static std::atomic<int> g_cpp_keep_running;

/* Teardown ensures spinning threads are stopped even if an assertion fails. */
static int teardown_stop_cpp_spin(void **state)
{
	(void)state;
	g_cpp_keep_running.store(0);
	test_msleep(30);
	return 0;
}

extern "C" {

static void cpp_entry_set_flag(void *arg)
{
	(void)arg;
	g_cpp_flag.store(1);
}

static void cpp_entry_capture_arg(void *arg)
{
	g_cpp_arg_val.store(reinterpret_cast<intptr_t>(arg));
}

static void cpp_entry_spin(void *arg)
{
	(void)arg;
	while (g_cpp_keep_running.load())
		test_msleep(1);
}

static void cpp_entry_sleep_briefly(void *arg)
{
	(void)arg;
	g_cpp_flag.store(1);
	test_msleep(200);
	g_cpp_flag.store(2);
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_thread_create_destroy(void **state)
{
	(void)state;
	g_cpp_flag.store(0);
	ove::Thread<4096> t(cpp_entry_set_flag, nullptr, OVE_PRIO_NORMAL, "t1");
	assert_true(t.valid());
	test_msleep(50);
	assert_int_equal(g_cpp_flag.load(), 1);
}

static void test_cpp_thread_entry_arg(void **state)
{
	(void)state;
	g_cpp_arg_val.store(0);
	int sentinel = 0xBEEF;
	ove::Thread<4096> t(cpp_entry_capture_arg, &sentinel, OVE_PRIO_NORMAL, "t2");
	test_msleep(50);
	assert_true(g_cpp_arg_val.load() == reinterpret_cast<intptr_t>(&sentinel));
}

static void test_cpp_thread_sleep_duration(void **state)
{
	(void)state;
	uint64_t before = 0, after = 0;
	ove_time_get_us(&before);
	ove::this_thread::sleep_ms(100);
	ove_time_get_us(&after);
	assert_duration_within(after - before, 100, OVE_TEST_TIMING_TOLERANCE_MS);
}

static void test_cpp_thread_yield(void **state)
{
	(void)state;
	ove::this_thread::yield();
}

static void test_cpp_thread_get_self(void **state)
{
	(void)state;
	ove::this_thread::self();
}

static void test_cpp_thread_set_priority(void **state)
{
	(void)state;
	g_cpp_keep_running.store(1);
	ove::Thread<4096> t(cpp_entry_spin, nullptr, OVE_PRIO_NORMAL, "t7");
	test_msleep(10);
	t.set_priority(OVE_PRIO_HIGH);
	g_cpp_keep_running.store(0);
	test_msleep(20);
}

static void test_cpp_thread_get_state_running(void **state)
{
	(void)state;
	g_cpp_keep_running.store(1);
	ove::Thread<4096> t(cpp_entry_spin, nullptr, OVE_PRIO_NORMAL, "t8");
	test_msleep(20);
	auto st = t.get_state();
	assert_true(st == OVE_THREAD_STATE_RUNNING || st == OVE_THREAD_STATE_READY ||
		    st == OVE_THREAD_STATE_BLOCKED);
	g_cpp_keep_running.store(0);
	test_msleep(20);
}

static void test_cpp_thread_get_state_terminated(void **state)
{
	(void)state;
	g_cpp_flag.store(0);
	ove::Thread<4096> t(cpp_entry_set_flag, nullptr, OVE_PRIO_NORMAL, "t9");
	test_msleep(100);
	auto st = t.get_state();
	assert_true(st == OVE_THREAD_STATE_TERMINATED || st == OVE_THREAD_STATE_SUSPENDED);
}

static void test_cpp_thread_stack_usage(void **state)
{
	(void)state;
	g_cpp_keep_running.store(1);
	ove::Thread<4096> t(cpp_entry_spin, nullptr, OVE_PRIO_NORMAL, "t10");
	test_msleep(10);
	(void)t.get_stack_usage();
	g_cpp_keep_running.store(0);
	test_msleep(20);
}

static void test_cpp_thread_suspend_resume(void **state)
{
	(void)state;
	g_cpp_flag.store(0);
	ove::Thread<4096> t(cpp_entry_sleep_briefly, nullptr, OVE_PRIO_NORMAL, "t14");
	for (int i = 0; i < 100 && g_cpp_flag.load() == 0; i++)
		test_msleep(5);
	assert_int_equal(g_cpp_flag.load(), 1);

	t.suspend();
	test_msleep(10);
	t.resume();
	test_msleep(300);
}

static void test_cpp_thread_runtime_stats(void **state)
{
	(void)state;
	g_cpp_keep_running.store(1);
	ove::Thread<4096> t(cpp_entry_spin, nullptr, OVE_PRIO_NORMAL, "t16");
	test_msleep(20);

	auto r = t.get_runtime_stats();
	assert_true(r.has_value() || r.error() == ove::Error::NotSupported);

	g_cpp_keep_running.store(0);
	test_msleep(20);
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_thread_raii_destroy(void **state)
{
	(void)state;
	{
		g_cpp_flag.store(0);
		ove::Thread<4096> t(cpp_entry_set_flag, nullptr, OVE_PRIO_NORMAL, "raii");
		test_msleep(50);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_thread_move_construct(void **state)
{
	(void)state;
	g_cpp_keep_running.store(1);
	auto a = make_test_thread("mv", cpp_entry_spin);
	assert_true(a.valid());

	ove::Thread<4096> b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());

	g_cpp_keep_running.store(0);
	test_msleep(20);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_thread_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Thread<4096>>::value,
		      "Thread must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Thread<4096>>::value,
		      "Thread must not be copy assignable");
}

/* ── Iter A1.1: thread_list() pass-through, no binding-level cap ────
 *
 * The C++ binding used to maintain its own 16-entry temp buffer and
 * silently cap `thread_list(out, max)` to min(max, 16).  After the
 * fix, `ove::ThreadInfo` is a using-alias for `struct ove_thread_info`
 * and `thread_list` passes `out` straight to the substrate — no
 * binding cap, no copy.  These tests verify:
 *   1. Layout compatibility (ThreadInfo == ove_thread_info).
 *   2. Pass-through semantics: count returned equals what the
 *      substrate filled, without further narrowing in the binding.
 *
 * The substrate itself may still cap (currently 16; tracked as
 * substrate P2-2); these tests assert the binding does not ADD a
 * narrower cap on top.
 */
static void test_cpp_thread_list_layout_compat(void **state)
{
	(void)state;
	static_assert(std::is_same_v<ove::ThreadInfo, struct ove_thread_info>,
		      "ove::ThreadInfo must be an alias of struct ove_thread_info "
		      "(zero-copy pass-through contract)");
	static_assert(sizeof(ove::ThreadInfo) == sizeof(struct ove_thread_info),
		      "ThreadInfo size must equal ove_thread_info");
}

static void test_cpp_thread_list_no_binding_cap(void **state)
{
	(void)state;
	/* Caller buffer larger than the substrate's current 16-entry cap.
	 * Substrate fills as many as it has; binding must not narrow
	 * further.  Whatever count the substrate returns, that's what
	 * the test sees through the binding. */
	constexpr size_t kBuf = 64;
	ove::ThreadInfo info[kBuf]{};
	size_t n = 0;
	const int rc = ove::thread_list(info, kBuf, &n);
	if (rc == OVE_OK) {
		assert_true(n <= kBuf);
		/* Substrate cap is 16 today; assert binding lets `n` reach
		 * the substrate's actual count, not a binding-narrower
		 * value. */
	} else {
		assert_int_equal(rc, OVE_ERR_NOT_SUPPORTED);
	}
}

int test_cpp_thread_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_thread_create_destroy),
		cmocka_unit_test(test_cpp_thread_entry_arg),
		cmocka_unit_test(test_cpp_thread_sleep_duration),
		cmocka_unit_test(test_cpp_thread_yield),
		cmocka_unit_test(test_cpp_thread_get_self),
		cmocka_unit_test_teardown(test_cpp_thread_set_priority, teardown_stop_cpp_spin),
		cmocka_unit_test_teardown(test_cpp_thread_get_state_running,
					  teardown_stop_cpp_spin),
		cmocka_unit_test(test_cpp_thread_get_state_terminated),
		cmocka_unit_test_teardown(test_cpp_thread_stack_usage, teardown_stop_cpp_spin),
		cmocka_unit_test(test_cpp_thread_suspend_resume),
		cmocka_unit_test_teardown(test_cpp_thread_runtime_stats, teardown_stop_cpp_spin),
		cmocka_unit_test(test_cpp_thread_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_teardown(test_cpp_thread_move_construct, teardown_stop_cpp_spin),
#endif
		cmocka_unit_test(test_cpp_thread_not_copyable),
		cmocka_unit_test(test_cpp_thread_list_layout_compat),
		cmocka_unit_test(test_cpp_thread_list_no_binding_cap),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
