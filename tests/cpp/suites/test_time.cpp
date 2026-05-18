#include "../framework/ove_test.hpp"

static void test_cpp_time_get_us(void **state)
{
	(void)state;
	assert_true(ove::time::get_us().has_value());
}

static void test_cpp_time_delay_ms(void **state)
{
	(void)state;
	const uint64_t before = ove::time::get_us().value_or(0);
	ove::time::delay_ms(50);
	const uint64_t after = ove::time::get_us().value_or(0);
	assert_true((after - before) >= 40000);
}

static void test_cpp_time_delay_us(void **state)
{
	(void)state;
	const uint64_t before = ove::time::get_us().value_or(0);
	ove::time::delay_us(10000);
	const uint64_t after = ove::time::get_us().value_or(0);
	assert_true((after - before) >= 5000);
}

int test_cpp_time_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_time_get_us),
		cmocka_unit_test(test_cpp_time_delay_ms),
		cmocka_unit_test(test_cpp_time_delay_us),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
