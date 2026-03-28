#include "../framework/ove_test.hpp"
#include "ove/infer.h"

/* Include the sine model data */
#include "../../models/sine_model.h"

#ifdef CONFIG_OVE_INFER

static void test_cpp_infer_create_destroy(void **state)
{
	(void)state;
	struct ove_model_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.model_data = sine_model_data;
	cfg.model_size = sine_model_data_len;
	cfg.arena_size = SINE_MODEL_ARENA_SIZE;

	ove_model_t model = nullptr;
	int rc = ove_model_create(&model, &cfg);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(model);
	ove_model_destroy(model);
}

static void test_cpp_infer_invoke(void **state)
{
	(void)state;
	struct ove_model_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.model_data = sine_model_data;
	cfg.model_size = sine_model_data_len;
	cfg.arena_size = SINE_MODEL_ARENA_SIZE;

	ove_model_t model = nullptr;
	ove_model_create(&model, &cfg);

	/* Get input tensor, set value */
	struct ove_tensor_info info;
	ove_model_input(model, 0, &info);
	float *input = static_cast<float *>(info.data);
	*input = 1.0f;

	int rc = ove_model_invoke(model);
	assert_int_equal(rc, OVE_OK);

	/* Check output */
	ove_model_output(model, 0, &info);
	float *output = static_cast<float *>(info.data);
	/* sin(1.0) ~ 0.841 */
	assert_true(*output > 0.5f && *output < 1.0f);

	assert_true(ove_model_last_inference_us(model) > 0);

	ove_model_destroy(model);
}

static void test_cpp_infer_null_params(void **state)
{
	(void)state;
	assert_int_not_equal(ove_model_create(nullptr, nullptr), OVE_OK);
	assert_int_not_equal(ove_model_invoke(nullptr), OVE_OK);
	assert_int_equal(ove_model_last_inference_us(nullptr), 0);
}

static void test_cpp_infer_tensor_out_of_bounds(void **state)
{
	(void)state;
	struct ove_model_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.model_data = sine_model_data;
	cfg.model_size = sine_model_data_len;
	cfg.arena_size = SINE_MODEL_ARENA_SIZE;

	ove_model_t model = nullptr;
	ove_model_create(&model, &cfg);

	struct ove_tensor_info info;
	assert_int_not_equal(ove_model_input(model, 999, &info), OVE_OK);
	assert_int_not_equal(ove_model_output(model, 999, &info), OVE_OK);

	ove_model_destroy(model);
}

#else

static void test_cpp_infer_stub(void **state)
{
	(void)state;
	assert_int_not_equal(ove_model_create(nullptr, nullptr), OVE_OK);
}

#endif

int test_cpp_infer_run(void)
{
	const struct CMUnitTest tests[] = {
#ifdef CONFIG_OVE_INFER
		cmocka_unit_test(test_cpp_infer_create_destroy),
		cmocka_unit_test(test_cpp_infer_invoke),
		cmocka_unit_test(test_cpp_infer_null_params),
		cmocka_unit_test(test_cpp_infer_tensor_out_of_bounds),
#else
		cmocka_unit_test(test_cpp_infer_stub),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
