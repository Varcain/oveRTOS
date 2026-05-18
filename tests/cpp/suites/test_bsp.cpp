#include "../framework/ove_test.hpp"

static void test_cpp_bsp_board_init(void **state)
{
	(void)state;
	assert_true(ove::bsp::board_init().has_value());
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
	assert_true(ove::bsp::gpio_set(0, 0, 1).has_value());
	assert_true(ove::bsp::gpio_get(0, 0).has_value());
}

static void test_cpp_bsp_gpio_irq(void **state)
{
	(void)state;
	assert_true(ove::bsp::gpio_irq_register(0, 0, OVE_GPIO_IRQ_RISING, nullptr, nullptr)
			    .has_value());
	assert_true(ove::bsp::gpio_irq_enable(0, 0).has_value());
	assert_true(ove::bsp::gpio_irq_disable(0, 0).has_value());
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
