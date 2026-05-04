#include "../framework/ove_test.hpp"

static std::atomic<int> s_cpp_oneshot_count;
static std::atomic<int> s_cpp_periodic_count;
static std::atomic<uintptr_t> s_cpp_user_data_received;

static void cpp_oneshot_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
	s_cpp_oneshot_count++;
}

static void cpp_periodic_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
	s_cpp_periodic_count++;
}

[[maybe_unused]] static void cpp_userdata_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	s_cpp_user_data_received = (uintptr_t)user_data;
}

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_timer_create_destroy_oneshot(void **state)
{
	(void)state;
	ove::Timer t(cpp_oneshot_cb, nullptr, 100, true);
	assert_true(t.valid());
}

static void test_cpp_timer_create_destroy_periodic(void **state)
{
	(void)state;
	ove::Timer t(cpp_periodic_cb, nullptr, 50, false);
	assert_true(t.valid());
}

/* Timer-firing tests skipped under TSan (gated at registration);
 * gate the function definitions too so -Werror=unused-function stays
 * happy on TSan builds. */
#ifndef __SANITIZE_THREAD__
static void test_cpp_timer_oneshot_fires_once(void **state)
{
	(void)state;
	s_cpp_oneshot_count = 0;

	ove::Timer t(cpp_oneshot_cb, nullptr, 30, true);
	(void)t.start();

	test_msleep(200);
	assert_int_equal(s_cpp_oneshot_count, 1);
}

static void test_cpp_timer_periodic_fires_multiple(void **state)
{
	(void)state;
	s_cpp_periodic_count = 0;

	ove::Timer t(cpp_periodic_cb, nullptr, 30, false);
	(void)t.start();

	test_msleep(250);
	(void)t.stop();
	assert_true(s_cpp_periodic_count >= 3);
}

static void test_cpp_timer_stop_prevents_callbacks(void **state)
{
	(void)state;
	s_cpp_periodic_count = 0;

	ove::Timer t(cpp_periodic_cb, nullptr, 20, false);
	(void)t.start();

	test_msleep(100);
	(void)t.stop();

	int count_after_stop = s_cpp_periodic_count;
	test_msleep(150);

	assert_true(s_cpp_periodic_count <= count_after_stop + 1);
}

static void test_cpp_timer_reset_restarts(void **state)
{
	(void)state;
	s_cpp_periodic_count = 0;

	ove::Timer t(cpp_periodic_cb, nullptr, 50, false);
	(void)t.start();

	test_msleep(80);
	int before_reset = s_cpp_periodic_count;
	(void)t.reset();

	test_msleep(200);
	assert_true(s_cpp_periodic_count > before_reset);
	(void)t.stop();
}

static void test_cpp_timer_double_start(void **state)
{
	(void)state;
	s_cpp_periodic_count = 0;

	ove::Timer t(cpp_periodic_cb, nullptr, 30, false);
	(void)t.start();
	(void)t.start(); /* should not crash */

	test_msleep(150);
	(void)t.stop();
	assert_true(s_cpp_periodic_count >= 2);
}

static void test_cpp_timer_destroy_while_running(void **state)
{
	(void)state;
	s_cpp_periodic_count = 0;

	{
		ove::Timer t(cpp_periodic_cb, nullptr, 20, false);
		(void)t.start();
		test_msleep(60);
		/* RAII destroy without explicit stop */
	}
	/* Let any in-flight SIGEV_THREAD callbacks drain before next test */
	test_msleep(50);
}

static void test_cpp_timer_callback_user_data(void **state)
{
	(void)state;
	s_cpp_user_data_received = 0;

	uintptr_t magic = 0xDEADBEEF;
	ove::Timer t(cpp_userdata_cb, (void *)magic, 20, true);
	(void)t.start();

	test_msleep(150);
	assert_true(s_cpp_user_data_received == magic);
}
#endif /* !__SANITIZE_THREAD__ */

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_timer_raii_destroy(void **state)
{
	(void)state;
#ifdef __SANITIZE_THREAD__
	skip(); /* Calls t.start() which triggers SIGEV_THREAD; same skip
		 * rationale as the firing tests above. */
#else
	{
		ove::Timer t(cpp_periodic_cb, nullptr, 50, false);
		(void)t.start();
		test_msleep(60);
	}
	/* Let any in-flight SIGEV_THREAD callbacks drain before next test */
	test_msleep(50);
#endif
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_timer_move_construct(void **state)
{
	(void)state;
	ove::Timer a(cpp_periodic_cb, nullptr, 50, false);
	assert_true(a.valid());

	ove::Timer b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_timer_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Timer>::value,
		      "Timer must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Timer>::value,
		      "Timer must not be copy assignable");
}

int test_cpp_timer_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_timer_create_destroy_oneshot),
		cmocka_unit_test(test_cpp_timer_create_destroy_periodic),
	/* Firing-dependent tests skipped under TSan — same SIGEV_THREAD
		 * runtime issue as the C side test_timer_run. */
#ifndef __SANITIZE_THREAD__
		cmocka_unit_test(test_cpp_timer_oneshot_fires_once),
		cmocka_unit_test(test_cpp_timer_periodic_fires_multiple),
		cmocka_unit_test(test_cpp_timer_stop_prevents_callbacks),
		cmocka_unit_test(test_cpp_timer_reset_restarts),
		cmocka_unit_test(test_cpp_timer_double_start),
		cmocka_unit_test(test_cpp_timer_destroy_while_running),
		cmocka_unit_test(test_cpp_timer_callback_user_data),
#endif
		cmocka_unit_test(test_cpp_timer_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_timer_move_construct),
#endif
		cmocka_unit_test(test_cpp_timer_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
