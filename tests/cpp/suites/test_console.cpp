#include "../framework/ove_test.hpp"
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>

static void test_cpp_console_init(void **state)
{
	(void)state;
	assert_true(ove::console::init().has_value());
}

static void test_cpp_console_putchar(void **state)
{
	(void)state;
	ove::console::putchar('A');
}

static void test_cpp_console_write(void **state)
{
	(void)state;
	ove::console::write("hello", 5);
}

static void test_cpp_console_getchar(void **state)
{
	(void)state;
	/* Save original stdin fd, redirect to /dev/null, then restore */
	int saved_stdin = dup(STDIN_FILENO);
	int devnull_fd = open("/dev/null", O_RDONLY);
	dup2(devnull_fd, STDIN_FILENO);
	close(devnull_fd);

	(void)ove::console::getchar();

	/* Restore original stdin */
	dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdin);
}

static void test_cpp_console_try_getchar(void **state)
{
	(void)state;
	int saved_stdin = dup(STDIN_FILENO);
	int devnull_fd = open("/dev/null", O_RDONLY);
	dup2(devnull_fd, STDIN_FILENO);
	close(devnull_fd);

	assert_int_equal(ove::console::try_getchar(), -1);

	dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdin);
}

int test_cpp_console_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_console_init),
		cmocka_unit_test(test_cpp_console_putchar),
		cmocka_unit_test(test_cpp_console_write),
		cmocka_unit_test(test_cpp_console_getchar),
		cmocka_unit_test(test_cpp_console_try_getchar),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
