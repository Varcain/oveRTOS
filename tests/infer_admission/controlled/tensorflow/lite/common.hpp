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
	kTfLiteFloat32 = 1,
	kTfLiteInt8 = 9,
	kTfLiteUInt8 = 3,
	kTfLiteInt16 = 7,
	kTfLiteInt32 = 2
};
struct TfLiteIntArray {
	int size;
	int data[5];
};
struct TfLiteTensor {
	union {
		char *raw;
	} data;
	size_t bytes;
	TfLiteType type;
	TfLiteIntArray *dims;
};
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
