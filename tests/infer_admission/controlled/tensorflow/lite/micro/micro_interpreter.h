// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tensorflow/lite/common.hpp"
#define TFLITE_SCHEMA_VERSION (3)
namespace tflite
{
class MicroAllocator
{
      public:
	uint8_t *arena;
	static MicroAllocator *Create(uint8_t *arena, size_t)
	{
		static MicroAllocator allocator;
		allocator.arena = arena;
		return &allocator;
	}
};
class MicroInterpreter
{
	TfLiteIntArray shape{1, {1}};
	TfLiteTensor in, out;

      public:
	MicroInterpreter(const Model *, const MicroOpResolver &, MicroAllocator *allocator)
		: in{{reinterpret_cast<char *>(allocator->arena)},
		     sizeof(float),
		     kTfLiteFloat32,
		     &shape},
		  out{{reinterpret_cast<char *>(allocator->arena + 16)},
		      sizeof(float),
		      kTfLiteFloat32,
		      &shape}
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
	size_t arena_used_bytes() const
	{
		return 32;
	}
};
} // namespace tflite
