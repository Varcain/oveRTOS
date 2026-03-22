#include "../framework/ove_test.hpp"

static void test_cpp_board_init(void **state)
{
	(void)state;
	int ret = ove::board::init();
	assert_int_equal(ret, OVE_OK);
}

static void test_cpp_board_name(void **state)
{
	(void)state;
	(void)ove::board::init();
	const char *name = ove::board::name();
	assert_non_null(name);
}

static void test_cpp_board_desc(void **state)
{
	(void)state;
	(void)ove::board::init();
	const struct ove_board_desc *desc = ove::board::desc();
	assert_non_null(desc);
	assert_non_null(desc->name);
	assert_true(desc->led_count > 0);
}

int test_cpp_board_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_board_init),
		cmocka_unit_test(test_cpp_board_name),
		cmocka_unit_test(test_cpp_board_desc),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
