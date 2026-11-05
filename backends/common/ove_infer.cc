/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * ove_infer — portable ML inference wrapper around TensorFlow Lite Micro.
 *
 * This file is C++ internally (TFLM requires it) but exports a pure C API
 * via extern "C".  It lives in backends/common/ because the implementation
 * is backend-agnostic — all RTOS differences are handled by the platform
 * port layer (ove_tflm_debug_log.cc, ove_tflm_time.cc) and oveRTOS
 * primitives (OVE_BACKEND_MALLOC, ove_time_get_us).
 */

extern "C" {
#include "ove/infer.h"
#include "ove/time.h"
#include "ove_backend_common.h"
}

#include <climits>
#include <cstring>
#include <limits>
#include <new>

#include "tflm/ove_tflm_arena.h"

#include "tensorflow/lite/micro/micro_arena_constants.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

/*
 * Op resolver with generous capacity — registers the most common ops
 * needed for typical TinyML models (classification, detection, audio).
 * MicroMutableOpResolver<N> is templated on max op count.
 */
static constexpr int kMaxOps = 64;
using OveOpResolver = tflite::MicroMutableOpResolver<kMaxOps>;

/*
 * Register common operators needed for most TinyML models.
 */
static TfLiteStatus register_common_ops(OveOpResolver &resolver)
{
	/* Core math */
	TF_LITE_ENSURE_STATUS(resolver.AddAdd());
	TF_LITE_ENSURE_STATUS(resolver.AddMul());
	TF_LITE_ENSURE_STATUS(resolver.AddSub());
	TF_LITE_ENSURE_STATUS(resolver.AddDiv());

	/* Convolutions */
	TF_LITE_ENSURE_STATUS(resolver.AddConv2D());
	TF_LITE_ENSURE_STATUS(resolver.AddDepthwiseConv2D());
	TF_LITE_ENSURE_STATUS(resolver.AddTransposeConv());

	/* Activations */
	TF_LITE_ENSURE_STATUS(resolver.AddRelu());
	TF_LITE_ENSURE_STATUS(resolver.AddRelu6());
	TF_LITE_ENSURE_STATUS(resolver.AddTanh());
	TF_LITE_ENSURE_STATUS(resolver.AddLogistic());
	TF_LITE_ENSURE_STATUS(resolver.AddSoftmax());

	/* Pooling */
	TF_LITE_ENSURE_STATUS(resolver.AddMaxPool2D());
	TF_LITE_ENSURE_STATUS(resolver.AddAveragePool2D());

	/* Fully connected */
	TF_LITE_ENSURE_STATUS(resolver.AddFullyConnected());

	/* Shape manipulation */
	TF_LITE_ENSURE_STATUS(resolver.AddReshape());
	TF_LITE_ENSURE_STATUS(resolver.AddConcatenation());
	TF_LITE_ENSURE_STATUS(resolver.AddPad());
	TF_LITE_ENSURE_STATUS(resolver.AddPadV2());
	TF_LITE_ENSURE_STATUS(resolver.AddSplit());
	TF_LITE_ENSURE_STATUS(resolver.AddSplitV());
	TF_LITE_ENSURE_STATUS(resolver.AddStridedSlice());
	TF_LITE_ENSURE_STATUS(resolver.AddSqueeze());
	TF_LITE_ENSURE_STATUS(resolver.AddExpandDims());
	TF_LITE_ENSURE_STATUS(resolver.AddTranspose());

	/* Quantization */
	TF_LITE_ENSURE_STATUS(resolver.AddQuantize());
	TF_LITE_ENSURE_STATUS(resolver.AddDequantize());

	/* Reduce */
	TF_LITE_ENSURE_STATUS(resolver.AddMean());
	TF_LITE_ENSURE_STATUS(resolver.AddMaximum());
	TF_LITE_ENSURE_STATUS(resolver.AddMinimum());

	/* Other common ops */
	TF_LITE_ENSURE_STATUS(resolver.AddNeg());
	TF_LITE_ENSURE_STATUS(resolver.AddRsqrt());
	TF_LITE_ENSURE_STATUS(resolver.AddCast());
	TF_LITE_ENSURE_STATUS(resolver.AddResizeBilinear());
	TF_LITE_ENSURE_STATUS(resolver.AddResizeNearestNeighbor());
	TF_LITE_ENSURE_STATUS(resolver.AddBatchMatMul());

	/* Signal processing ops (used by audio preprocessor models) */
	TF_LITE_ENSURE_STATUS(resolver.AddWindow());
	TF_LITE_ENSURE_STATUS(resolver.AddFftAutoScale());
	TF_LITE_ENSURE_STATUS(resolver.AddRfft());
	TF_LITE_ENSURE_STATUS(resolver.AddEnergy());
	TF_LITE_ENSURE_STATUS(resolver.AddFilterBank());
	TF_LITE_ENSURE_STATUS(resolver.AddFilterBankSquareRoot());
	TF_LITE_ENSURE_STATUS(resolver.AddFilterBankSpectralSubtraction());
	TF_LITE_ENSURE_STATUS(resolver.AddPCAN());
	TF_LITE_ENSURE_STATUS(resolver.AddFilterBankLog());

	return kTfLiteOk;
}

/*
 * Internal: publish a tensor descriptor only when it is fully representable.
 */
static int fill_tensor_info(const TfLiteTensor *tensor, struct ove_tensor_info *info)
{
	enum ove_tensor_type type;
	switch (tensor->type) {
	case kTfLiteFloat32:
		type = OVE_TENSOR_FLOAT32;
		break;
	case kTfLiteInt8:
		type = OVE_TENSOR_INT8;
		break;
	case kTfLiteUInt8:
		type = OVE_TENSOR_UINT8;
		break;
	case kTfLiteInt16:
		type = OVE_TENSOR_INT16;
		break;
	case kTfLiteInt32:
		type = OVE_TENSOR_INT32;
		break;
	default:
		return OVE_ERR_NOT_SUPPORTED;
	}

	const unsigned int ndims = (unsigned int)tensor->dims->size;
	if (ndims > 5)
		return OVE_ERR_NOT_SUPPORTED;

	info->data = tensor->data.raw;
	info->size = tensor->bytes;
	info->type = type;
	info->ndims = ndims;
	for (unsigned int i = 0; i < ndims; i++)
		info->dims[i] = tensor->dims->data[i];
	for (unsigned int i = ndims; i < 5; i++)
		info->dims[i] = 0;
	return OVE_OK;
}

/*
 * TFLM object allocation strategy:
 *
 * C++ operator new on bare-metal ARM goes through newlib malloc/_sbrk,
 * which is separate from the RTOS heap (pvPortMalloc).  We use static
 * byte arrays for the resolver and interpreter objects — only one model
 * is active at a time, so they can be reused.  This works in both heap
 * and zero-heap modes and avoids all dynamic allocation.
 */
static uint8_t __attribute__((aligned(8))) s_resolver_buf[sizeof(OveOpResolver)];
static uint8_t __attribute__((aligned(8))) s_interpreter_buf[sizeof(tflite::MicroInterpreter)];

/* Admission owns both placement buffers through complete teardown. Keep only
 * a boolean: a forgotten synchronous session must not leave an owner pointer
 * that a later constructor could access after its backing has gone away. */
static unsigned char s_model_claimed;

static bool model_try_claim()
{
	unsigned char idle = 0;
	return __atomic_compare_exchange_n(&s_model_claimed, &idle, 1, false, __ATOMIC_ACQUIRE,
					   __ATOMIC_RELAXED);
}

static void model_release()
{
	__atomic_store_n(&s_model_claimed, 0, __ATOMIC_RELEASE);
}

static int model_config_status(const struct ove_model_config *cfg)
{
	if (!cfg || !cfg->model_data || cfg->model_size == 0 ||
	    cfg->arena_size > static_cast<size_t>(INT_MAX))
		return OVE_ERR_INVALID_PARAM;
	const uintptr_t bytes = reinterpret_cast<uintptr_t>(cfg->model_data);
	if ((bytes & (tflite::MicroArenaBufferAlignment() - 1)) ||
	    cfg->model_size > std::numeric_limits<uintptr_t>::max() - bytes)
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

static size_t model_arena_padding(const void *arena)
{
	return (-reinterpret_cast<uintptr_t>(arena)) & (tflite::MicroArenaBufferAlignment() - 1);
}

static int model_arena_status(const void *arena, size_t bytes)
{
	if (bytes > std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(arena))
		return OVE_ERR_INVALID_PARAM;
	const size_t padding = model_arena_padding(arena);
	if (padding > bytes || bytes - padding < ove_tflm_arena_minimum())
		return OVE_ERR_NO_MEMORY;
	return OVE_OK;
}

/*
 * Internal: set up the TFLM interpreter inside a struct ove_model.
 * Assumes m->model_data, m->arena, m->arena_size are already set.
 */
static int model_setup(struct ove_model *m)
{
	const int arena_status = model_arena_status(m->arena, m->arena_size);
	if (arena_status != OVE_OK)
		return arena_status;
	const tflite::Model *tfl_model = tflite::GetModel(m->model_data);
	if (!tfl_model)
		return OVE_ERR_ML_FAILED;
	if (tfl_model->version() != TFLITE_SCHEMA_VERSION)
		return OVE_ERR_NOT_SUPPORTED;

	const size_t padding = model_arena_padding(m->arena);
	auto *effective = m->arena + padding;
	const size_t effective_size = m->arena_size - padding;
	/* Unvisited node records must retain null registrations during a partial
	 * setup failure. Clear before any allocator or callback places its data. */
	std::memset(effective, 0, effective_size);
	const OveTflmArena arena = ove_tflm_arena_create(effective, effective_size);
	if (!arena.allocator)
		return OVE_ERR_NO_MEMORY;

	auto *resolver = new (s_resolver_buf) OveOpResolver();
	if (register_common_ops(*resolver) != kTfLiteOk) {
		resolver->~OveOpResolver();
		return OVE_ERR_ML_FAILED;
	}
	m->resolver = resolver;

	auto *interpreter = new (s_interpreter_buf)
		tflite::MicroInterpreter(tfl_model, *resolver, arena.allocator);

	TfLiteStatus status = interpreter->AllocateTensors();
	if (status != kTfLiteOk) {
		const bool exhausted = ove_tflm_arena_exhausted(arena);
		interpreter->~MicroInterpreter();
		resolver->~OveOpResolver();
		m->resolver = nullptr;
		return exhausted ? OVE_ERR_NO_MEMORY : OVE_ERR_ML_FAILED;
	}

	m->interpreter = interpreter;
	m->last_invoke_us = 0;
	return OVE_OK;
}

/*
 * Internal: tear down the TFLM interpreter objects.
 * Calls destructors but does not free memory (static buffers).
 */
static bool model_teardown(struct ove_model *m)
{
	if (!m->interpreter)
		return false;
	auto *interpreter = static_cast<tflite::MicroInterpreter *>(m->interpreter);
	auto *resolver = static_cast<OveOpResolver *>(m->resolver);
	/* Consume the published live session before calling its destructors.
	 * Closing an already-closed static header cannot release another lease. */
	m->interpreter = nullptr;
	m->resolver = nullptr;
	interpreter->~MicroInterpreter();
	resolver->~OveOpResolver();
	return true;
}

/* ── Public C API ────────────────────────────────────────────────────── */

extern "C" {

int ove_model_init(ove_model_t *model, ove_model_storage_t *storage, void *arena,
		   const struct ove_model_config *cfg)
{
	if (!model || !storage || !arena)
		return OVE_ERR_INVALID_PARAM;
	int checked = model_config_status(cfg);
	if (checked != OVE_OK)
		return checked;
	if (cfg->arena_size >
	    std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(arena))
		return OVE_ERR_INVALID_PARAM;
	if (!model_try_claim())
		return OVE_ERR_BUSY;
	checked = model_arena_status(arena, cfg->arena_size);
	if (checked != OVE_OK) {
		model_release();
		return checked;
	}

	struct ove_model *m = storage;
	m->model_data = cfg->model_data;
	m->model_size = cfg->model_size;
	m->arena = static_cast<uint8_t *>(arena);
	m->arena_size = cfg->arena_size;
	m->interpreter = nullptr;
	m->resolver = nullptr;
	m->heap_allocated = 0;

	int rc = model_setup(m);
	if (rc != OVE_OK) {
		model_release();
		return rc;
	}

	*model = m;
	return OVE_OK;
}

void ove_model_deinit(ove_model_t model)
{
	if (!model)
		return;
	if (model_teardown(model))
		model_release();
}

#ifdef OVE_HEAP_INFER

int ove_model_create(ove_model_t *model, const struct ove_model_config *cfg)
{
	if (!model)
		return OVE_ERR_INVALID_PARAM;
	const int checked = model_config_status(cfg);
	if (checked != OVE_OK)
		return checked;
	if (!model_try_claim())
		return OVE_ERR_BUSY;
	if (cfg->arena_size < ove_tflm_arena_minimum()) {
		model_release();
		return OVE_ERR_NO_MEMORY;
	}

	auto *m = static_cast<struct ove_model *>(OVE_BACKEND_MALLOC(sizeof(struct ove_model)));
	if (!m) {
		model_release();
		return OVE_ERR_NO_MEMORY;
	}

	auto *arena = static_cast<uint8_t *>(OVE_BACKEND_MALLOC(cfg->arena_size));
	if (!arena) {
		OVE_BACKEND_FREE(m);
		model_release();
		return OVE_ERR_NO_MEMORY;
	}

	m->model_data = cfg->model_data;
	m->model_size = cfg->model_size;
	m->arena = arena;
	m->arena_size = cfg->arena_size;
	m->interpreter = nullptr;
	m->resolver = nullptr;
	m->heap_allocated = 1;

	int rc = model_setup(m);
	if (rc != OVE_OK) {
		OVE_BACKEND_FREE(arena);
		OVE_BACKEND_FREE(m);
		model_release();
		return rc;
	}

	*model = m;
	return OVE_OK;
}

void ove_model_destroy(ove_model_t model)
{
	if (!model)
		return;
	bool consumed = model_teardown(model);
	if (model->heap_allocated) {
		OVE_BACKEND_FREE(model->arena);
		OVE_BACKEND_FREE(model);
	}
	/* No header access follows its free; even a competing initializer must
	 * wait until every destructor and owned-allocation access has ended. */
	if (consumed)
		model_release();
}

#endif /* OVE_HEAP_INFER */

int ove_model_invoke(ove_model_t model)
{
	if (!model || !model->interpreter)
		return OVE_ERR_INVALID_PARAM;

	auto *interp = static_cast<tflite::MicroInterpreter *>(model->interpreter);

	uint64_t start = 0;
	ove_time_get_us(&start);

	TfLiteStatus status = interp->Invoke();

	uint64_t end = 0;
	ove_time_get_us(&end);
	model->last_invoke_us = end - start;

	return (status == kTfLiteOk) ? OVE_OK : OVE_ERR_ML_FAILED;
}

int ove_model_input(ove_model_t model, unsigned int index, struct ove_tensor_info *info)
{
	if (!model || !model->interpreter || !info)
		return OVE_ERR_INVALID_PARAM;

	auto *interp = static_cast<tflite::MicroInterpreter *>(model->interpreter);

	if (index >= (unsigned int)interp->inputs_size())
		return OVE_ERR_INVALID_PARAM;

	TfLiteTensor *tensor = interp->input(index);
	if (!tensor)
		return OVE_ERR_ML_FAILED;

	return fill_tensor_info(tensor, info);
}

int ove_model_output(ove_model_t model, unsigned int index, struct ove_tensor_info *info)
{
	if (!model || !model->interpreter || !info)
		return OVE_ERR_INVALID_PARAM;

	auto *interp = static_cast<tflite::MicroInterpreter *>(model->interpreter);

	if (index >= (unsigned int)interp->outputs_size())
		return OVE_ERR_INVALID_PARAM;

	const TfLiteTensor *tensor = interp->output(index);
	if (!tensor)
		return OVE_ERR_ML_FAILED;

	return fill_tensor_info(tensor, info);
}

uint64_t ove_model_last_inference_us(ove_model_t model)
{
	if (!model)
		return 0;
	return model->last_invoke_us;
}

size_t ove_model_arena_used(ove_model_t model)
{
	if (!model || !model->interpreter)
		return 0;
	const auto *interpreter = static_cast<const tflite::MicroInterpreter *>(model->interpreter);
	return interpreter->arena_used_bytes() + model_arena_padding(model->arena);
}

} /* extern "C" */
