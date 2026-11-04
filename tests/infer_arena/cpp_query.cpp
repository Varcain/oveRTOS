// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ove/infer.hpp"
#include "ove/console.h"
#include "ove/time.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "models/sine_model.h"
#include <cstdio>
extern "C" int ove_time_get_us(uint64_t *value)
{
	*value = 0;
	return OVE_OK;
}
extern "C" void ove_console_write(const char *, unsigned)
{
}
static ove::ModelStorage<8192> storage;
int main()
{
	auto model = storage.load(sine_model_data, sizeof(sine_model_data));
	if (!model)
		return 1;
	auto *interpreter = static_cast<tflite::MicroInterpreter *>(model->handle()->interpreter);
	if (!model->arena_used() || model->arena_used() != interpreter->arena_used_bytes())
		return 1;
	std::printf("C++ query used=%zu PASS\n", model->arena_used());
	return 0;
}
