// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <ove/infer.h>
#include <ove/console.h>
#include <ove/time.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include <cstdio>
#include <cstring>

// Only the controlled SDK sees the token and mutable tensor records. No real
// model parser, kernel or Invoke consumes these descriptor-conversion cases.
alignas(16) static const unsigned char model_token[16] = {1};
alignas(16) static unsigned char arena[256];
alignas(16) static unsigned char data[2][256];
static ove_model_storage_t storage{};
static TfLiteIntArray shapes[2]{};
static TfLiteTensor tensors[2]{};
static bool missing[2]{};
static int resolver_count, interpreter_count;
static unsigned int cases, failures, controls, rejections;

extern "C" int infer_probe_get_model()
{
	return 1;
}
extern "C" int infer_probe_register()
{
	return 0;
}
extern "C" int infer_probe_allocate()
{
	return 0;
}
extern "C" void infer_probe_resolver(int delta)
{
	resolver_count += delta;
}
extern "C" void infer_probe_interpreter(int delta)
{
	interpreter_count += delta;
}
extern "C" TfLiteTensor *infer_probe_tensor(unsigned int direction)
{
	return missing[direction] ? nullptr : &tensors[direction];
}
extern "C" int ove_time_get_us(uint64_t *out)
{
	*out = 0;
	return OVE_OK;
}
extern "C" void ove_console_write(const char *, unsigned int)
{
}

struct Type {
	TfLiteType native;
	bool supported;
	ove_tensor_type mapped;
	size_t bytes;
};

static constexpr Type types[] = {
	{kTfLiteNoType, false, OVE_TENSOR_FLOAT32, 1},
	{kTfLiteFloat32, true, OVE_TENSOR_FLOAT32, 4},
	{kTfLiteInt32, true, OVE_TENSOR_INT32, 4},
	{kTfLiteUInt8, true, OVE_TENSOR_UINT8, 1},
	{kTfLiteInt64, false, OVE_TENSOR_FLOAT32, 8},
	{kTfLiteString, false, OVE_TENSOR_FLOAT32, 1},
	{kTfLiteBool, false, OVE_TENSOR_FLOAT32, 1},
	{kTfLiteInt16, true, OVE_TENSOR_INT16, 2},
	{kTfLiteComplex64, false, OVE_TENSOR_FLOAT32, 8},
	{kTfLiteInt8, true, OVE_TENSOR_INT8, 1},
	{kTfLiteFloat16, false, OVE_TENSOR_FLOAT32, 2},
	{kTfLiteFloat64, false, OVE_TENSOR_FLOAT32, 8},
	{kTfLiteComplex128, false, OVE_TENSOR_FLOAT32, 16},
	{kTfLiteUInt64, false, OVE_TENSOR_FLOAT32, 8},
	{kTfLiteResource, false, OVE_TENSOR_FLOAT32, 1},
	{kTfLiteVariant, false, OVE_TENSOR_FLOAT32, 1},
	{kTfLiteUInt32, false, OVE_TENSOR_FLOAT32, 4},
	{kTfLiteUInt16, false, OVE_TENSOR_FLOAT32, 2},
	{kTfLiteInt4, false, OVE_TENSOR_FLOAT32, 1},
	{kTfLiteBFloat16, false, OVE_TENSOR_FLOAT32, 2},
	{kTfLiteInt2, false, OVE_TENSOR_FLOAT32, 1},
	{kTfLiteUInt4, false, OVE_TENSOR_FLOAT32, 1},
};

static void configure(unsigned int direction, TfLiteType type, int rank, size_t width)
{
	constexpr int dimensions[] = {1, 2, 1, 3, 1, 1};
	shapes[direction].size = rank;
	size_t elements = 1;
	for (unsigned int i = 0; i < 6; ++i) {
		shapes[direction].data[i] = dimensions[i];
		if (i < static_cast<unsigned int>(rank))
			elements *= static_cast<size_t>(dimensions[i]);
	}
	tensors[direction] = {{reinterpret_cast<char *>(data[direction])},
			      elements * width,
			      type,
			      &shapes[direction]};
	missing[direction] = false;
}

static ove_tensor_info sentinel()
{
	return {data[0] + 128, 17, OVE_TENSOR_INT32, 5, {9, 8, 7, 6, 5}};
}

static bool equal(const ove_tensor_info &left, const ove_tensor_info &right)
{
	if (left.data != right.data || left.size != right.size || left.type != right.type ||
	    left.ndims != right.ndims)
		return false;
	for (unsigned int i = 0; i < 5; ++i)
		if (left.dims[i] != right.dims[i])
			return false;
	return true;
}

static int query(ove_model_t model, unsigned int direction, unsigned int index,
		 ove_tensor_info *info)
{
	return direction ? ove_model_output(model, index, info)
			 : ove_model_input(model, index, info);
}

static void observe(bool passed, const char *name, unsigned int direction, TfLiteType type,
		    int rank, int actual, int expected)
{
	++cases;
	if (expected == OVE_ERR_NOT_SUPPORTED)
		++rejections;
	else
		++controls;
	if (!passed) {
		++failures;
		std::printf("FAIL %s direction=%u type=%d rank=%d status=%d expected=%d\n", name,
			    direction, static_cast<int>(type), rank, actual, expected);
	}
}

static void matrix(ove_model_t model)
{
	for (unsigned int direction = 0; direction < 2; ++direction) {
		for (const auto &type : types) {
			for (int rank = 0; rank <= 6; ++rank) {
				configure(1 - direction, kTfLiteUInt8, 1, 1);
				configure(direction, type.native, rank, type.bytes);
				const auto before = sentinel();
				auto actual = before;
				const int expected = type.supported && rank <= 5
							     ? OVE_OK
							     : OVE_ERR_NOT_SUPPORTED;
				const int rc = query(model, direction, 0, &actual);
				bool exact = equal(actual, before);
				if (expected == OVE_OK) {
					ove_tensor_info wanted{data[direction],
							       tensors[direction].bytes,
							       type.mapped,
							       static_cast<unsigned int>(rank),
							       {}};
					for (int i = 0; i < rank; ++i)
						wanted.dims[i] = shapes[direction].data[i];
					exact = equal(actual, wanted);
				}
				observe(rc == expected && exact, "metadata", direction, type.native,
					rank, rc, expected);
			}
		}

		// Rejection does not close or poison the session. Recover with distinct,
		// valid tensor records and preserve the existing argument/lookup errors.
		configure(direction, kTfLiteFloat32, 1, 4);
		const auto before = sentinel();
		auto actual = before;
		int rc = query(model, direction, 0, &actual);
		ove_tensor_info wanted{data[direction], 4, OVE_TENSOR_FLOAT32, 1, {1, 0, 0, 0, 0}};
		observe(rc == OVE_OK && equal(actual, wanted), "recovery", direction,
			kTfLiteFloat32, 1, rc, OVE_OK);

		ove_model_storage_t closed{};
		for (unsigned int argument = 0; argument < 4; ++argument) {
			actual = before;
			ove_model_t handle = argument == 0   ? nullptr
					     : argument == 1 ? &closed
							     : model;
			rc = query(handle, direction, argument == 2 ? 1 : 0,
				   argument == 3 ? nullptr : &actual);
			observe(rc == OVE_ERR_INVALID_PARAM && equal(actual, before), "arguments",
				direction, kTfLiteFloat32, 1, rc, OVE_ERR_INVALID_PARAM);
		}
		missing[direction] = true;
		actual = before;
		rc = query(model, direction, 0, &actual);
		observe(rc == OVE_ERR_ML_FAILED && equal(actual, before), "missing", direction,
			kTfLiteFloat32, 1, rc, OVE_ERR_ML_FAILED);
		missing[direction] = false;
	}
}

static bool run(bool heap)
{
	cases = failures = controls = rejections = 0;
	const ove_model_config cfg{model_token, sizeof(model_token), sizeof(arena)};
	ove_model_t model = nullptr;
	int rc;
#ifdef OVE_HEAP_INFER
	if (heap)
		rc = ove_model_create(&model, &cfg);
	else
#endif
		rc = ove_model_init(&model, &storage, arena, &cfg);
	if (rc != OVE_OK)
		return false;
	matrix(model);
#ifdef OVE_HEAP_INFER
	if (heap)
		ove_model_destroy(model);
	else
#endif
		ove_model_deinit(model);
	std::printf("tensor descriptors %s: %u cases, %u failures (%u controls, %u rejections)\n",
		    heap ? "create" : "init", cases, failures, controls, rejections);
	return cases == 320 && controls == 72 && rejections == 248 && !failures &&
	       !resolver_count && !interpreter_count;
}

int main(int argc, char **argv)
{
	if (argc != 2 || std::strcmp(argv[1], "matrix") != 0)
		return 2;
	bool passed = run(false);
#ifdef OVE_HEAP_INFER
	passed = run(true) && passed;
#endif
	return passed ? 0 : 1;
}
