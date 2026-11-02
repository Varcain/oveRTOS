// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <ove/infer.h>
#include <ove/time.h>
#include <ove/console.h>
#include "models/sine_model.h"
#include <cstdio>
#include <cstring>
#include <ctime>
extern "C" int ove_time_get_us(uint64_t *value)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	*value = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
	return OVE_OK;
}
extern "C" void ove_console_write(const char *data, unsigned int size)
{
	fwrite(data, 1, size, stderr);
}
static struct ove_model_config config()
{
	return {sine_model_data, sizeof(sine_model_data), SINE_MODEL_ARENA_SIZE};
}
static int invoke(ove_model_t model)
{
	ove_tensor_info input{}, output{};
	if (ove_model_input(model, 0, &input) != OVE_OK || input.type != OVE_TENSOR_FLOAT32 ||
	    input.size != sizeof(float))
		return 1;
	*static_cast<float *>(input.data) = 1.0f;
	if (ove_model_invoke(model) != OVE_OK || ove_model_output(model, 0, &output) != OVE_OK)
		return 2;
	float result = *static_cast<float *>(output.data);
	return result > 0.5f && result < 1.0f ? 0 : 3;
}
alignas(16) static uint8_t first_arena[65536], second_arena[65536];
static ove_model_storage_t first_storage{}, second_storage{};
static int construct(bool heap, ove_model_t *out, ove_model_storage_t *storage, void *arena,
		     const ove_model_config *cfg)
{
#ifdef OVE_HEAP_INFER
	if (heap)
		return ove_model_create(out, cfg);
#else
	if (heap)
		return OVE_ERR_NOT_SUPPORTED;
#endif
	return ove_model_init(out, storage, arena, cfg);
}
static void close(bool heap, ove_model_t model)
{
#ifdef OVE_HEAP_INFER
	if (heap) {
		ove_model_destroy(model);
		return;
	}
#else
	(void)heap;
#endif
	ove_model_deinit(model);
}
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	auto cfg = config();
	cfg.arena_size = sizeof(first_arena);
	if (!strncmp(argv[1], "sequential", 10)) {
		bool heap = strstr(argv[1], "heap") != nullptr;
		for (unsigned i = 0; i < 3; i++) {
			ove_model_t m = nullptr;
			if (construct(heap, &m, &first_storage, first_arena, &cfg) != OVE_OK)
				return 3;
			int result = invoke(m);
			close(heap, m);
			if (result)
				return 4;
		}
		puts("valid sequential actual TFLM sessions pass");
		return 0;
	}
	bool first_heap = !strncmp(argv[1], "heap-", 5);
	bool second_heap = strstr(argv[1], "-heap") != nullptr;
	ove_model_t first = nullptr;
	if (construct(first_heap, &first, &first_storage, first_arena, &cfg) != OVE_OK)
		return 5;
	// Retain all first-session backing. Never access or destroy its handle.
	first = nullptr;
	ove_model_t second = nullptr;
	int result = construct(second_heap, &second, &second_storage, second_arena, &cfg);
	if (result == OVE_OK) {
		int valid = invoke(second);
		close(second_heap, second);
		if (valid)
			return 6;
	}
	printf("second constructor result=%d; expected Busy=%d\n", result, OVE_ERR_BUSY);
	return result == OVE_ERR_BUSY ? 0 : 1;
}
