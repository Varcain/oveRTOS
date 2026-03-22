#include "../framework/ove_test.hpp"

static void test_cpp_bsp_board_init(void **state)
{
	(void)state;
	int ret = ove::bsp::board_init();
	assert_int_equal(ret, OVE_OK);
}

static void test_cpp_bsp_led_set(void **state)
{
	(void)state;
	ove::bsp::led_set(0, 1);
	ove::bsp::led_set(0, 0);
}

static void test_cpp_bsp_led_toggle(void **state)
{
	(void)state;
	ove::bsp::led_toggle(0);
}

static void test_cpp_bsp_gpio_set_get(void **state)
{
	(void)state;
	int ret = ove::bsp::gpio_set(0, 0, 1);
	assert_int_equal(ret, OVE_OK);

	ret = ove::bsp::gpio_get(0, 0);
	/* Returns pin value (0 or 1) or error */
	assert_true(ret >= 0);
}

static void test_cpp_bsp_gpio_irq(void **state)
{
	(void)state;
	int ret = ove::bsp::gpio_irq_register(0, 0, OVE_GPIO_IRQ_RISING,
						    nullptr, nullptr);
	assert_int_equal(ret, OVE_OK);

	ret = ove::bsp::gpio_irq_enable(0, 0);
	assert_int_equal(ret, OVE_OK);

	ret = ove::bsp::gpio_irq_disable(0, 0);
	assert_int_equal(ret, OVE_OK);
}

int test_cpp_bsp_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_bsp_board_init),
		cmocka_unit_test(test_cpp_bsp_led_set),
		cmocka_unit_test(test_cpp_bsp_led_toggle),
		cmocka_unit_test(test_cpp_bsp_gpio_set_get),
		cmocka_unit_test(test_cpp_bsp_gpio_irq),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
