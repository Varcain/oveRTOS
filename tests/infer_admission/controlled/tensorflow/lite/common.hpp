// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
extern "C" {
int infer_probe_get_model();
int infer_probe_register();
void infer_probe_resolver(int delta);
void infer_probe_interpreter(int delta);
int infer_probe_allocate();
}
enum TfLiteStatus { kTfLiteOk = 0, kTfLiteError = 1 };
enum TfLiteType {
	kTfLiteNoType = 0,
	kTfLiteFloat32 = 1,
	kTfLiteInt32 = 2,
	kTfLiteUInt8 = 3,
	kTfLiteInt64 = 4,
	kTfLiteString = 5,
	kTfLiteBool = 6,
	kTfLiteInt16 = 7,
	kTfLiteComplex64 = 8,
	kTfLiteInt8 = 9,
	kTfLiteFloat16 = 10,
	kTfLiteFloat64 = 11,
	kTfLiteComplex128 = 12,
	kTfLiteUInt64 = 13,
	kTfLiteResource = 14,
	kTfLiteVariant = 15,
	kTfLiteUInt32 = 16,
	kTfLiteUInt16 = 17,
	kTfLiteInt4 = 18,
	kTfLiteBFloat16 = 19,
	kTfLiteInt2 = 20,
	kTfLiteUInt4 = 21
};
struct TfLiteIntArray {
	int size;
	int data[6];
};
struct TfLiteTensor {
	union {
		char *raw;
	} data;
	size_t bytes;
	TfLiteType type;
	TfLiteIntArray *dims;
};
#ifdef OVE_INFER_DESCRIPTOR_PROBE
extern "C" TfLiteTensor *infer_probe_tensor(unsigned int direction);
#endif
#define TF_LITE_ENSURE_STATUS(expression)   \
	do {                                \
		auto status = (expression); \
		if (status != kTfLiteOk)    \
			return status;      \
	} while (false)
namespace tflite
{
struct Model {
	uint32_t version() const
	{
		return 3;
	}
};
struct MicroOpResolver {
};
inline const Model *GetModel(const void *data)
{
	(void)data;
	static const Model model{};
	return infer_probe_get_model() ? &model : nullptr;
}
} // namespace tflite
