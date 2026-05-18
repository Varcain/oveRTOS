#include "../framework/ove_test.hpp"

static void test_cpp_gpio_set_get(void **state)
{
	(void)state;
	assert_true(ove::gpio::set(0, 0, 1).has_value());
	assert_true(ove::gpio::get(0, 0).has_value());
}

static void test_cpp_gpio_irq(void **state)
{
	(void)state;
	assert_true(
		ove::gpio::irq_register(0, 0, OVE_GPIO_IRQ_RISING, nullptr, nullptr).has_value());
	assert_true(ove::gpio::irq_enable(0, 0).has_value());
	assert_true(ove::gpio::irq_disable(0, 0).has_value());
}

int test_cpp_gpio_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_gpio_set_get),
		cmocka_unit_test(test_cpp_gpio_irq),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
