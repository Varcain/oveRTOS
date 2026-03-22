#include "../framework/ove_test.hpp"

static void test_cpp_gpio_set_get(void **state)
{
	(void)state;
	int ret = ove::gpio::set(0, 0, 1);
	assert_int_equal(ret, OVE_OK);

	ret = ove::gpio::get(0, 0);
	/* Returns pin value (0 or 1) or error */
	assert_true(ret >= 0);
}

static void test_cpp_gpio_irq(void **state)
{
	(void)state;
	int ret = ove::gpio::irq_register(0, 0, OVE_GPIO_IRQ_RISING,
					       nullptr, nullptr);
	assert_int_equal(ret, OVE_OK);

	ret = ove::gpio::irq_enable(0, 0);
	assert_int_equal(ret, OVE_OK);

	ret = ove::gpio::irq_disable(0, 0);
	assert_int_equal(ret, OVE_OK);
}

int test_cpp_gpio_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_gpio_set_get),
		cmocka_unit_test(test_cpp_gpio_irq),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
