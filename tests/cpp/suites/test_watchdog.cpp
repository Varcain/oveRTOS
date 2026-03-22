#include "../framework/ove_test.hpp"

static void test_cpp_watchdog_create_destroy(void **state)
{
	(void)state;
	ove::Watchdog w(1000);
	assert_true(w.valid());
}

static void test_cpp_watchdog_start_stop(void **state)
{
	(void)state;
	ove::Watchdog w(1000);

	assert_int_equal(w.start(), OVE_OK);
	assert_int_equal(w.feed(), OVE_OK);
	assert_int_equal(w.stop(), OVE_OK);
}

static void test_cpp_watchdog_feed(void **state)
{
	(void)state;
	ove::Watchdog w(500);
	(void)w.start();

	for (int i = 0; i < 3; i++) {
		test_msleep(100);
		assert_int_equal(w.feed(), OVE_OK);
	}

	(void)w.stop();
}

static void test_cpp_watchdog_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Watchdog w(1000);
		(void)w.start();
	}
}

static void test_cpp_watchdog_move_construct(void **state)
{
	(void)state;
	ove::Watchdog a(1000);
	assert_true(a.valid());

	ove::Watchdog b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}

static void test_cpp_watchdog_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Watchdog>::value,
		      "Watchdog must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Watchdog>::value,
		      "Watchdog must not be copy assignable");
}

int test_cpp_watchdog_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_watchdog_create_destroy),
		cmocka_unit_test(test_cpp_watchdog_start_stop),
		cmocka_unit_test(test_cpp_watchdog_feed),
		cmocka_unit_test(test_cpp_watchdog_raii_destroy),
		cmocka_unit_test(test_cpp_watchdog_move_construct),
		cmocka_unit_test(test_cpp_watchdog_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
