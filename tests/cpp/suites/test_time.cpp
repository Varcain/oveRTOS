#include "../framework/ove_test.hpp"

static void test_cpp_time_get_us(void **state)
{
	(void)state;
	uint64_t us = 0;
	int ret = ove::time::get_us(&us);
	assert_int_equal(ret, OVE_OK);
}

static void test_cpp_time_delay_ms(void **state)
{
	(void)state;
	uint64_t before = 0, after = 0;
	(void)ove::time::get_us(&before);
	ove::time::delay_ms(50);
	(void)ove::time::get_us(&after);
	assert_true((after - before) >= 40000);
}

static void test_cpp_time_delay_us(void **state)
{
	(void)state;
	uint64_t before = 0, after = 0;
	(void)ove::time::get_us(&before);
	ove::time::delay_us(10000);
	(void)ove::time::get_us(&after);
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
