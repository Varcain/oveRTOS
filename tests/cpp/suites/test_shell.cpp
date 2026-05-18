#include "../framework/ove_test.hpp"

static int s_shell_called;
static void test_shell_handler(int argc, const char *argv[])
{
	(void)argc;
	(void)argv;
	s_shell_called++;
}

static void test_cpp_shell_init(void **state)
{
	(void)state;
	assert_true(ove::shell::init().has_value());
}

static void test_cpp_shell_register_cmd(void **state)
{
	(void)state;
	(void)ove::shell::init();

	struct ove_shell_cmd cmd = {"test", "a test command", test_shell_handler};
	assert_true(ove::shell::register_cmd(&cmd).has_value());
}

static void test_cpp_shell_process_char(void **state)
{
	(void)state;
	ove::shell::process_char('a');
}

int test_cpp_shell_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_shell_init),
		cmocka_unit_test(test_cpp_shell_register_cmd),
		cmocka_unit_test(test_cpp_shell_process_char),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
