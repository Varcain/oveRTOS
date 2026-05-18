#include "../framework/ove_test.hpp"

static void test_cpp_nvs_init_deinit(void **state)
{
	(void)state;
	assert_true(ove::nvs::init().has_value());
	ove::nvs::deinit();
}

static void test_cpp_nvs_write_read(void **state)
{
	(void)state;
	(void)ove::nvs::init();

	uint32_t val = 0xDEADBEEF;
	assert_true(ove::nvs::write("test_key", &val, sizeof(val)).has_value());

	uint32_t out = 0;
	auto r = ove::nvs::read("test_key", &out, sizeof(out));
	assert_true(r.has_value());
	assert_int_equal(*r, sizeof(val));
	assert_true(out == 0xDEADBEEF);

	ove::nvs::deinit();
}

static void test_cpp_nvs_erase(void **state)
{
	(void)state;
	(void)ove::nvs::init();

	uint32_t val = 42;
	(void)ove::nvs::write("erase_key", &val, sizeof(val));
	assert_true(ove::nvs::erase("erase_key").has_value());

	ove::nvs::deinit();
}

int test_cpp_nvs_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_nvs_init_deinit),
		cmocka_unit_test(test_cpp_nvs_write_read),
		cmocka_unit_test(test_cpp_nvs_erase),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
