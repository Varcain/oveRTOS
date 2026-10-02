#include "../framework/ove_test.hpp"

static void test_cpp_gpio_set_get(void **state)
{
	(void)state;
	assert_true(ove::gpio::set(0, 0, 1).has_value());
	assert_true(ove::gpio::get(0, 0).has_value());
}

static void gpio_irq_cb(unsigned int, unsigned int, void *)
{
}

static void test_cpp_gpio_irq(void **state)
{
	(void)state;
	assert_true(ove::gpio::irq_register(0, 0, OVE_GPIO_IRQ_RISING, gpio_irq_cb, nullptr)
			    .has_value());
	assert_true(ove::gpio::irq_enable(0, 0).has_value());
	assert_true(ove::gpio::irq_disable(0, 0).has_value());
	assert_true(ove::gpio::irq_unregister(0, 0).has_value());
}

static void test_cpp_gpio_irq_single_owner(void **state)
{
	(void)state;
	assert_true(ove::gpio::irq_register(0, 0, OVE_GPIO_IRQ_RISING, gpio_irq_cb, nullptr)
			    .has_value());
	auto again = ove::gpio::irq_register(0, 0, OVE_GPIO_IRQ_FALLING, gpio_irq_cb, nullptr);
	assert_false(again.has_value());
	assert_true(again.error() == ove::Error::AlreadyExists);
	assert_true(ove::gpio::irq_unregister(0, 0).has_value());
	assert_true(ove::gpio::irq_register(0, 0, OVE_GPIO_IRQ_FALLING, gpio_irq_cb, nullptr)
			    .has_value());
	assert_true(ove::gpio::irq_unregister(0, 0).has_value());
	assert_false(ove::gpio::irq_unregister(0, 0).has_value());
}

static void test_cpp_gpio_irq_rejects_null_callback(void **state)
{
	(void)state;
	auto r = ove::gpio::irq_register(0, 0, OVE_GPIO_IRQ_RISING, nullptr, nullptr);
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::InvalidParam);
}

int test_cpp_gpio_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_gpio_set_get),
		cmocka_unit_test(test_cpp_gpio_irq),
		cmocka_unit_test(test_cpp_gpio_irq_single_owner),
		cmocka_unit_test(test_cpp_gpio_irq_rejects_null_callback),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
