// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tensorflow/lite/common.hpp"
namespace tflite
{
class MicroInterpreter
{
	TfLiteIntArray shape{1, {1}};
	TfLiteTensor in, out;

      public:
	MicroInterpreter(const Model *, const MicroOpResolver &, uint8_t *arena, size_t)
		: in{{reinterpret_cast<char *>(arena)}, sizeof(float), kTfLiteFloat32, &shape},
		  out{{reinterpret_cast<char *>(arena + 16)}, sizeof(float), kTfLiteFloat32, &shape}
	{
		infer_probe_interpreter(1);
	}
	~MicroInterpreter()
	{
		infer_probe_interpreter(-1);
	}
	TfLiteStatus AllocateTensors()
	{
		return static_cast<TfLiteStatus>(infer_probe_allocate());
	}
	TfLiteStatus Invoke()
	{
		return kTfLiteOk;
	}
	int inputs_size() const
	{
		return 1;
	}
	int outputs_size() const
	{
		return 1;
	}
	TfLiteTensor *input(unsigned)
	{
		return &in;
	}
	const TfLiteTensor *output(unsigned)
	{
		return &out;
	}
};
} // namespace tflite
