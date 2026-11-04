/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ove/infer.h"
#include "framework/semihosting_exit.h"
#include "g_audio_preprocessor_int8_model_data.h"
#include "g_micro_speech_quantized_model_data.h"
#include <stdio.h>
#include <string.h>

enum { CAPACITY = 32768, FRAMES = 49, FEATURES = 40, SAMPLES = 480, SCORES = 4 };
static uint8_t arena[CAPACITY] __attribute__((aligned(16)));
static ove_model_storage_t storage;
static int8_t features[FRAMES][FEATURES];

static int tensor_matches(const struct ove_tensor_info *t, enum ove_tensor_type type, size_t bytes,
			  unsigned int rank, const int *dims)
{
	if (!t->data || t->type != type || t->size != bytes || t->ndims != rank)
		return 0;
	for (unsigned int i = 0; i < rank; ++i)
		if (t->dims[i] != dims[i])
			return 0;
	return 1;
}

static int open_model(ove_model_t *model, const void *bytes, size_t size, int heap)
{
	struct ove_model_config cfg = {
		.model_data = bytes,
		.model_size = size,
		.arena_size = CAPACITY,
	};
#ifdef OVE_HEAP_INFER
	if (heap)
		return ove_model_create(model, &cfg);
#else
	(void)heap;
#endif
	return ove_model_init(model, &storage, arena, &cfg);
}

static void close_model(ove_model_t model, int heap)
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

static int usage(ove_model_t model, size_t *maximum)
{
	size_t used = ove_model_arena_used(model);
	if (!used || used > CAPACITY || CAPACITY - used < 16) {
		printf("IT_FAILURE capacity=%u used=%u\n", CAPACITY, (unsigned int)used);
		return 1;
	}
	if (used > *maximum)
		*maximum = used;
	return 0;
}

static void dump_bytes(const int8_t *bytes, size_t count)
{
	for (size_t i = 0; i < count; ++i)
		printf("%02x", (unsigned int)(uint8_t)bytes[i]);
	putchar('\n');
}

static int sequence(unsigned int seed, int heap)
{
	const int pre_in[] = {1, SAMPLES}, pre_out[] = {FEATURES};
	const int cls_in[] = {1, FRAMES * FEATURES}, cls_out[] = {1, SCORES};
	struct ove_tensor_info input, output;
	ove_model_t model = NULL;
	size_t maximum = 0;
	if (open_model(&model, g_audio_preprocessor_int8_model_data,
		       g_audio_preprocessor_int8_model_data_size, heap) != OVE_OK)
		return 1;
	int failed = usage(model, &maximum) || ove_model_input(model, 0, &input) != OVE_OK ||
		     ove_model_output(model, 0, &output) != OVE_OK;
	if (!failed)
		failed = !tensor_matches(&input, OVE_TENSOR_INT16, SAMPLES * sizeof(int16_t), 2,
					 pre_in) ||
			 !tensor_matches(&output, OVE_TENSOR_INT8, FEATURES, 1, pre_out);
	for (unsigned int frame = 0; !failed && frame < FRAMES; ++frame) {
		int16_t *pcm = input.data;
		for (unsigned int i = 0; i < SAMPLES; ++i)
			pcm[i] = (int16_t)(100 + ((i + frame + seed) % 31));
		failed = ove_model_invoke(model) != OVE_OK || usage(model, &maximum);
		if (!failed) {
			memcpy(features[frame], output.data, FEATURES);
			printf("IT_FRAME %u %u ", seed, frame);
			dump_bytes(features[frame], FEATURES);
		}
	}
	if (!failed)
		printf("IT_ARENA %u preprocessor %u %u %u\n", seed, CAPACITY, (unsigned int)maximum,
		       (unsigned int)(-(uintptr_t)model->arena & 15));
	close_model(model, heap);
	if (failed)
		return 1;
	if (open_model(&model, g_micro_speech_quantized_model_data,
		       g_micro_speech_quantized_model_data_size, heap) != OVE_OK)
		return 1;
	maximum = 0;
	failed = usage(model, &maximum) || ove_model_input(model, 0, &input) != OVE_OK ||
		 ove_model_output(model, 0, &output) != OVE_OK;
	if (!failed)
		failed = !tensor_matches(&input, OVE_TENSOR_INT8, sizeof(features), 2, cls_in) ||
			 !tensor_matches(&output, OVE_TENSOR_INT8, SCORES, 2, cls_out);
	if (!failed) {
		memcpy(input.data, features, sizeof(features));
		failed = ove_model_invoke(model) != OVE_OK || usage(model, &maximum);
	}
	if (!failed) {
		printf("IT_SCORES %u ", seed);
		dump_bytes(output.data, SCORES);
		printf("IT_ARENA %u classifier %u %u %u\n", seed, CAPACITY, (unsigned int)maximum,
		       (unsigned int)(-(uintptr_t)model->arena & 15));
	}
	close_model(model, heap);
	return failed;
}

void ove_main(void)
{
	int failed = 0;
	for (int heap = 0; heap <= 1; ++heap) {
#ifndef OVE_HEAP_INFER
		if (heap)
			break;
#endif
		printf("IT_BEGIN %s\n", heap ? "create" : "init");
		failed = sequence(0, heap) || sequence(17, heap);
		if (failed)
			break;
		printf("IT_END\n");
	}
	printf("IT_RESULT %d\n", failed);
	fflush(stdout);
	semihosting_exit(failed);
}
