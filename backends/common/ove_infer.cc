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

#include <new>

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
 * Internal: map TfLiteType to ove_tensor_type.
 */
static enum ove_tensor_type tflite_type_to_ove(TfLiteType t)
{
	switch (t) {
	case kTfLiteFloat32: return OVE_TENSOR_FLOAT32;
	case kTfLiteInt8:    return OVE_TENSOR_INT8;
	case kTfLiteUInt8:   return OVE_TENSOR_UINT8;
	case kTfLiteInt16:   return OVE_TENSOR_INT16;
	case kTfLiteInt32:   return OVE_TENSOR_INT32;
	default:             return OVE_TENSOR_FLOAT32;
	}
}

/*
 * Internal: populate ove_tensor_info from a TfLiteTensor.
 */
static void fill_tensor_info(const TfLiteTensor *tensor,
			     struct ove_tensor_info *info)
{
	info->data = tensor->data.raw;
	info->size = tensor->bytes;
	info->type = tflite_type_to_ove(tensor->type);
	info->ndims = (unsigned int)tensor->dims->size;
	for (unsigned int i = 0; i < info->ndims && i < 5; i++)
		info->dims[i] = tensor->dims->data[i];
	for (unsigned int i = info->ndims; i < 5; i++)
		info->dims[i] = 0;
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
static uint8_t __attribute__((aligned(8)))
	s_resolver_buf[sizeof(OveOpResolver)];
static uint8_t __attribute__((aligned(8)))
	s_interpreter_buf[sizeof(tflite::MicroInterpreter)];

/*
 * Internal: set up the TFLM interpreter inside a struct ove_model.
 * Assumes m->model_data, m->arena, m->arena_size are already set.
 */
static int model_setup(struct ove_model *m)
{
	const tflite::Model *tfl_model =
		tflite::GetModel(m->model_data);
	if (!tfl_model)
		return OVE_ERR_ML_FAILED;

	auto *resolver = new (s_resolver_buf) OveOpResolver();
	if (register_common_ops(*resolver) != kTfLiteOk) {
		resolver->~OveOpResolver();
		return OVE_ERR_ML_FAILED;
	}
	m->resolver = resolver;

	auto *interpreter = new (s_interpreter_buf) tflite::MicroInterpreter(
		tfl_model, *resolver, m->arena, m->arena_size);

	TfLiteStatus status = interpreter->AllocateTensors();
	if (status != kTfLiteOk) {
		interpreter->~MicroInterpreter();
		resolver->~OveOpResolver();
		m->resolver = nullptr;
		return OVE_ERR_ML_FAILED;
	}

	m->interpreter = interpreter;
	m->last_invoke_us = 0;
	return OVE_OK;
}

/*
 * Internal: tear down the TFLM interpreter objects.
 * Calls destructors but does not free memory (static buffers).
 */
static void model_teardown(struct ove_model *m)
{
	if (m->interpreter) {
		static_cast<tflite::MicroInterpreter *>(m->interpreter)
			->~MicroInterpreter();
		m->interpreter = nullptr;
	}
	if (m->resolver) {
		static_cast<OveOpResolver *>(m->resolver)->~OveOpResolver();
		m->resolver = nullptr;
	}
}

/* ── Public C API ────────────────────────────────────────────────────── */

extern "C" {

int ove_model_init(ove_model_t *model, ove_model_storage_t *storage,
		   void *arena, const struct ove_model_config *cfg)
{
	if (!model || !storage || !arena || !cfg || !cfg->model_data)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->arena_size == 0 || cfg->model_size == 0)
		return OVE_ERR_INVALID_PARAM;

	struct ove_model *m = storage;
	m->model_data = cfg->model_data;
	m->model_size = cfg->model_size;
	m->arena = static_cast<uint8_t *>(arena);
	m->arena_size = cfg->arena_size;
	m->interpreter = nullptr;
	m->resolver = nullptr;
	m->heap_allocated = 0;

	int rc = model_setup(m);
	if (rc != OVE_OK)
		return rc;

	*model = m;
	return OVE_OK;
}

void ove_model_deinit(ove_model_t model)
{
	if (!model)
		return;
	model_teardown(model);
}

#ifdef OVE_HEAP_INFER

int ove_model_create(ove_model_t *model,
		     const struct ove_model_config *cfg)
{
	if (!model || !cfg || !cfg->model_data)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->arena_size == 0 || cfg->model_size == 0)
		return OVE_ERR_INVALID_PARAM;

	auto *m = static_cast<struct ove_model *>(
		OVE_BACKEND_MALLOC(sizeof(struct ove_model)));
	if (!m)
		return OVE_ERR_NO_MEMORY;

	auto *arena = static_cast<uint8_t *>(
		OVE_BACKEND_MALLOC(cfg->arena_size));
	if (!arena) {
		OVE_BACKEND_FREE(m);
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
		return rc;
	}

	*model = m;
	return OVE_OK;
}

void ove_model_destroy(ove_model_t model)
{
	if (!model)
		return;
	model_teardown(model);
	if (model->heap_allocated) {
		OVE_BACKEND_FREE(model->arena);
		OVE_BACKEND_FREE(model);
	}
}

#endif /* OVE_HEAP_INFER */

int ove_model_invoke(ove_model_t model)
{
	if (!model || !model->interpreter)
		return OVE_ERR_INVALID_PARAM;

	auto *interp = static_cast<tflite::MicroInterpreter *>(
		model->interpreter);

	uint64_t start = 0;
	ove_time_get_us(&start);

	TfLiteStatus status = interp->Invoke();

	uint64_t end = 0;
	ove_time_get_us(&end);
	model->last_invoke_us = end - start;

	return (status == kTfLiteOk) ? OVE_OK : OVE_ERR_ML_FAILED;
}

int ove_model_input(ove_model_t model, unsigned int index,
		    struct ove_tensor_info *info)
{
	if (!model || !model->interpreter || !info)
		return OVE_ERR_INVALID_PARAM;

	auto *interp = static_cast<tflite::MicroInterpreter *>(
		model->interpreter);

	if (index >= (unsigned int)interp->inputs_size())
		return OVE_ERR_INVALID_PARAM;

	TfLiteTensor *tensor = interp->input(index);
	if (!tensor)
		return OVE_ERR_ML_FAILED;

	fill_tensor_info(tensor, info);
	return OVE_OK;
}

int ove_model_output(ove_model_t model, unsigned int index,
		     struct ove_tensor_info *info)
{
	if (!model || !model->interpreter || !info)
		return OVE_ERR_INVALID_PARAM;

	auto *interp = static_cast<tflite::MicroInterpreter *>(
		model->interpreter);

	if (index >= (unsigned int)interp->outputs_size())
		return OVE_ERR_INVALID_PARAM;

	const TfLiteTensor *tensor = interp->output(index);
	if (!tensor)
		return OVE_ERR_ML_FAILED;

	fill_tensor_info(tensor, info);
	return OVE_OK;
}

uint64_t ove_model_last_inference_us(ove_model_t model)
{
	if (!model)
		return 0;
	return model->last_invoke_us;
}

} /* extern "C" */
