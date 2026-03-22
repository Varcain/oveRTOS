#include "../framework/ove_test.hpp"

static void test_cpp_nvs_init_deinit(void **state)
{
	(void)state;
	int ret = ove::nvs::init();
	assert_int_equal(ret, OVE_OK);
	ove::nvs::deinit();
}

static void test_cpp_nvs_write_read(void **state)
{
	(void)state;
	(void)ove::nvs::init();

	uint32_t val = 0xDEADBEEF;
	int ret = ove::nvs::write("test_key", &val, sizeof(val));
	assert_int_equal(ret, OVE_OK);

	uint32_t out = 0;
	size_t out_len = 0;
	ret = ove::nvs::read("test_key", &out, sizeof(out), &out_len);
	assert_int_equal(ret, OVE_OK);
	assert_int_equal(out_len, sizeof(val));
	assert_true(out == 0xDEADBEEF);

	ove::nvs::deinit();
}

static void test_cpp_nvs_erase(void **state)
{
	(void)state;
	(void)ove::nvs::init();

	uint32_t val = 42;
	(void)ove::nvs::write("erase_key", &val, sizeof(val));
	int ret = ove::nvs::erase("erase_key");
	assert_int_equal(ret, OVE_OK);

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
