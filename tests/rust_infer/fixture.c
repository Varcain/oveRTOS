// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <ove/infer.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* Initialized metadata only. No parser, kernel, native allocator or worker runs. */
_Alignas(16) const unsigned char infer_fixture_model_data[64] = {1};
const unsigned int infer_fixture_model_data_len = sizeof(infer_fixture_model_data);
_Alignas(16) static struct ove_model heap_header;
_Alignas(16) static unsigned char heap_arena[512];
_Alignas(16) static unsigned char other_data[64];
static unsigned char interpreter_marker;
static _Atomic bool occupied;
static _Atomic bool interrupt_context;
static _Atomic unsigned int selected;
static _Atomic unsigned int failure;
static _Atomic size_t configured_capacity;
static _Atomic unsigned int counts[8];

enum {
	NORMAL,
	WRONG_TYPE,
	PARTIAL_ELEMENT,
	OUTSIDE_ARENA,
	CONSTANT_OUTPUT,
	CONSTANT_INPUT,
	EMPTY,
	NULL_DATA,
	MISALIGNED,
	TOO_LARGE,
	STRADDLE,
};
enum { INIT, CREATE, DEINIT, DESTROY, ALLOC, FREE, INVOKE, WRITES };

static void count(unsigned int index)
{
	atomic_fetch_add(&counts[index], 1);
}

void infer_test_select(unsigned int value)
{
	atomic_store(&selected, value);
}

void infer_test_interrupt(bool value)
{
	atomic_store(&interrupt_context, value);
}

void infer_test_failure(unsigned int value)
{
	atomic_store(&failure, value);
}

unsigned int infer_test_count(unsigned int index)
{
	return index < 8 ? atomic_load(&counts[index]) : 0;
}

bool infer_test_occupied(void)
{
	return atomic_load(&occupied);
}

size_t infer_test_capacity(void)
{
	return atomic_load(&configured_capacity);
}

/* Only ordinary released cases use reset; retention cases end their process. */
void infer_test_reset(void)
{
	atomic_store(&occupied, false);
	atomic_store(&interrupt_context, false);
	atomic_store(&selected, NORMAL);
	atomic_store(&failure, 0);
	for (unsigned int i = 0; i < 8; ++i)
		atomic_store(&counts[i], 0);
	memset(&heap_header, 0, sizeof(heap_header));
	memset(heap_arena, 0, sizeof(heap_arena));
	memset(other_data, 0, sizeof(other_data));
}

static int install(ove_model_t *out, ove_model_storage_t *storage, void *arena,
		   const struct ove_model_config *config, bool heap)
{
	if (!out || !storage || !arena || !config || !config->model_data ||
	    config->model_size != sizeof(infer_fixture_model_data) || !config->arena_size ||
	    config->arena_size > sizeof(heap_arena))
		return OVE_ERR_INVALID_PARAM;
	bool expected = false;
	if (!atomic_compare_exchange_strong(&occupied, &expected, true))
		return OVE_ERR_BUSY;
	if (config->arena_size < 64 || atomic_load(&failure) == 1) {
		atomic_store(&occupied, false);
		return OVE_ERR_NO_MEMORY;
	}
	struct ove_model *model = storage;
	memset(model, 0, sizeof(*model));
	memset(arena, 0, config->arena_size);
	model->model_data = config->model_data;
	model->model_size = config->model_size;
	model->arena = arena;
	model->arena_size = config->arena_size;
	model->interpreter = &interpreter_marker;
	model->heap_allocated = heap;
	atomic_store(&configured_capacity, config->arena_size);
	count(WRITES);
	if (heap)
		count(ALLOC);
	*out = model;
	return OVE_OK;
}

int ove_model_init(ove_model_t *model, ove_model_storage_t *storage, void *arena,
		   const struct ove_model_config *config)
{
	count(INIT);
	return install(model, storage, arena, config, false);
}

static void close_model(ove_model_t model)
{
	if (!model || !model->interpreter)
		return;
	model->interpreter = NULL;
	atomic_store(&occupied, false);
}

void ove_model_deinit(ove_model_t model)
{
	count(DEINIT);
	close_model(model);
}

#ifdef OVE_HEAP_INFER
int ove_model_create(ove_model_t *model, const struct ove_model_config *config)
{
	count(CREATE);
	return install(model, &heap_header, heap_arena, config, true);
}

void ove_model_destroy(ove_model_t model)
{
	count(DESTROY);
	bool heap = model && model->interpreter && model->heap_allocated;
	close_model(model);
	if (heap)
		count(FREE);
}
#endif

int ove_model_binding_arena(ove_model_t model, void **base, size_t *capacity)
{
	if (!model || !model->interpreter || !base || !capacity)
		return OVE_ERR_INVALID_PARAM;
	size_t padding = (16 - ((uintptr_t)model->arena & 15)) & 15;
	*base = model->arena + padding;
	*capacity = model->arena_size - padding;
	return OVE_OK;
}

bool ove_model_binding_in_isr(void)
{
	return atomic_load(&interrupt_context);
}

static int descriptor(ove_model_t model, unsigned int index, struct ove_tensor_info *info,
		      bool output)
{
	if (!model || !model->interpreter || !info || index)
		return OVE_ERR_INVALID_PARAM;
	void *base;
	size_t capacity;
	int status = ove_model_binding_arena(model, &base, &capacity);
	if (status != OVE_OK)
		return status;
	unsigned int variant = atomic_load(&selected);
	struct ove_tensor_info value = {
		.data = (unsigned char *)base + (output ? 32 : 0),
		.size = 16,
		.type = variant == WRONG_TYPE ? OVE_TENSOR_INT8 : OVE_TENSOR_INT32,
		.ndims = 1,
		.dims = {4},
	};
	if (variant == WRONG_TYPE)
		value.dims[0] = 16;
	if (variant == PARTIAL_ELEMENT)
		value.size = 7;
	if (variant == OUTSIDE_ARENA)
		value.data = other_data;
	if ((variant == CONSTANT_OUTPUT && output) || (variant == CONSTANT_INPUT && !output))
		value.data = (unsigned char *)model->model_data + 16;
	if (variant == EMPTY) {
		value.data = NULL;
		value.size = 0;
		value.dims[0] = 0;
	}
	if (variant == NULL_DATA)
		value.data = NULL;
	if (variant == MISALIGNED)
		value.data = (unsigned char *)base + 1;
	if (variant == TOO_LARGE)
		value.size = (size_t)PTRDIFF_MAX + 1;
	if (variant == STRADDLE) {
		value.data = (unsigned char *)base + capacity - 4;
		value.size = 8;
	}
	*info = value;
	return OVE_OK;
}

int ove_model_input(ove_model_t model, unsigned int index, struct ove_tensor_info *info)
{
	return descriptor(model, index, info, false);
}

int ove_model_output(ove_model_t model, unsigned int index, struct ove_tensor_info *info)
{
	return descriptor(model, index, info, true);
}

int ove_model_invoke(ove_model_t model)
{
	if (!model || !model->interpreter)
		return OVE_ERR_INVALID_PARAM;
	count(INVOKE);
	unsigned char *base = model->arena + ((16 - ((uintptr_t)model->arena & 15)) & 15);
	memcpy(base + 32, base, 16);
	model->last_invoke_us = 7;
	return atomic_load(&failure) == 2 ? OVE_ERR_ML_FAILED : OVE_OK;
}

uint64_t ove_model_last_inference_us(ove_model_t model)
{
	return model ? model->last_invoke_us : 0;
}

size_t ove_model_arena_used(ove_model_t model)
{
	return model && model->interpreter ? 64 : 0;
}
