#include "../framework/ove_test.hpp"

static void test_cpp_led_set(void **state)
{
	(void)state;
	ove::led::set(0, 1);
	ove::led::set(0, 0);
}

static void test_cpp_led_toggle(void **state)
{
	(void)state;
	ove::led::toggle(0);
}

static void test_cpp_led_count(void **state)
{
	(void)state;
	unsigned int count = ove::led::count();
	assert_true(count > 0);
}

int test_cpp_led_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_led_set),
		cmocka_unit_test(test_cpp_led_toggle),
		cmocka_unit_test(test_cpp_led_count),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
