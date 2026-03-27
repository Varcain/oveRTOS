/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "../models/sine_model.h"

#ifdef CONFIG_OVE_INFER

/* Static storage for zero-heap test path */
OVE_TEST_STORAGE(ove_model_storage_t, s_model_storage);
static uint8_t __attribute__((aligned(16))) s_arena[SINE_MODEL_ARENA_SIZE];

static void test_model_create_destroy(void **state)
{
	(void)state;
	ove_model_t model = NULL;
	struct ove_model_config cfg = {
		.model_data = sine_model_data,
		.model_size = sine_model_data_len,
		.arena_size = SINE_MODEL_ARENA_SIZE,
	};

#ifdef CONFIG_OVE_ZERO_HEAP
	int rc = ove_model_init(&model, &s_model_storage, s_arena, &cfg);
#else
	int rc = ove_model_create(&model, &cfg);
#endif
	assert_int_equal(rc, OVE_OK);
	assert_non_null(model);

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_model_deinit(model);
#else
	ove_model_destroy(model);
#endif
}

static void test_model_invoke(void **state)
{
	(void)state;
	ove_model_t model = NULL;
	struct ove_model_config cfg = {
		.model_data = sine_model_data,
		.model_size = sine_model_data_len,
		.arena_size = SINE_MODEL_ARENA_SIZE,
	};

#ifdef CONFIG_OVE_ZERO_HEAP
	int rc = ove_model_init(&model, &s_model_storage, s_arena, &cfg);
#else
	int rc = ove_model_create(&model, &cfg);
#endif
	assert_int_equal(rc, OVE_OK);

	/* Populate input tensor */
	struct ove_tensor_info input_info;
	rc = ove_model_input(model, 0, &input_info);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(input_info.data);

	/* Set input value (e.g., 1.0) */
	if (input_info.type == OVE_TENSOR_FLOAT32) {
		float *in = (float *)input_info.data;
		*in = 1.0f;
	}

	/* Run inference */
	rc = ove_model_invoke(model);
	assert_int_equal(rc, OVE_OK);

	/* Check output tensor is accessible */
	struct ove_tensor_info output_info;
	rc = ove_model_output(model, 0, &output_info);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(output_info.data);

	/* Verify inference timing was recorded */
	uint64_t us = ove_model_last_inference_us(model);
	assert_true(us > 0);

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_model_deinit(model);
#else
	ove_model_destroy(model);
#endif
}

static void test_model_tensor_info(void **state)
{
	(void)state;
	ove_model_t model = NULL;
	struct ove_model_config cfg = {
		.model_data = sine_model_data,
		.model_size = sine_model_data_len,
		.arena_size = SINE_MODEL_ARENA_SIZE,
	};

#ifdef CONFIG_OVE_ZERO_HEAP
	int rc = ove_model_init(&model, &s_model_storage, s_arena, &cfg);
#else
	int rc = ove_model_create(&model, &cfg);
#endif
	assert_int_equal(rc, OVE_OK);

	/* Input tensor metadata */
	struct ove_tensor_info info;
	rc = ove_model_input(model, 0, &info);
	assert_int_equal(rc, OVE_OK);
	assert_true(info.ndims > 0);
	assert_true(info.size > 0);
	assert_non_null(info.data);

	/* Output tensor metadata */
	rc = ove_model_output(model, 0, &info);
	assert_int_equal(rc, OVE_OK);
	assert_true(info.ndims > 0);
	assert_true(info.size > 0);
	assert_non_null(info.data);

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_model_deinit(model);
#else
	ove_model_destroy(model);
#endif
}

static void test_model_null_params(void **state)
{
	(void)state;

	/* NULL model pointer */
	struct ove_model_config cfg = {
		.model_data = sine_model_data,
		.model_size = sine_model_data_len,
		.arena_size = SINE_MODEL_ARENA_SIZE,
	};
	int rc;

#ifdef CONFIG_OVE_ZERO_HEAP
	rc = ove_model_init(NULL, &s_model_storage, s_arena, &cfg);
#else
	rc = ove_model_create(NULL, &cfg);
#endif
	assert_int_not_equal(rc, OVE_OK);

	/* NULL config */
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_model_t model = NULL;
	rc = ove_model_init(&model, &s_model_storage, s_arena, NULL);
#else
	ove_model_t model = NULL;
	rc = ove_model_create(&model, NULL);
#endif
	assert_int_not_equal(rc, OVE_OK);
}

static void test_model_invoke_null(void **state)
{
	(void)state;
	int rc = ove_model_invoke(NULL);
	assert_int_not_equal(rc, OVE_OK);
}

static void test_model_last_inference_null(void **state)
{
	(void)state;
	uint64_t us = ove_model_last_inference_us(NULL);
	assert_int_equal((int)us, 0);
}

int test_infer_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_model_create_destroy),
		cmocka_unit_test(test_model_invoke),
		cmocka_unit_test(test_model_tensor_info),
		cmocka_unit_test(test_model_null_params),
		cmocka_unit_test(test_model_invoke_null),
		cmocka_unit_test(test_model_last_inference_null),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

#else /* !CONFIG_OVE_INFER */

int test_infer_run(void)
{
	return 0;
}

#endif /* CONFIG_OVE_INFER */
