/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file infer.h
 * @defgroup ove_infer ML Inference
 * @brief Portable inference API for running TFLite models via LiteRT
 *        (formerly TensorFlow Lite Micro).
 *
 * Provides a C API for loading pre-trained @c .tflite FlatBuffer models
 * and running inference on them.  The same model binary runs unchanged
 * across all four oveRTOS backends (FreeRTOS, Zephyr, NuttX, POSIX).
 *
 * Two allocation strategies are available:
 *  - @c _create() / @c _destroy() — heap-allocated, including the tensor arena.
 *    Available only when @c OVE_HEAP_INFER is defined (i.e.
 *    @c CONFIG_OVE_ZERO_HEAP is not set).
 *  - @c _init() / @c _deinit() — caller-supplied storage and arena buffer.
 *    Available in both modes.  See @c OVE_MODEL_DEFINE_STATIC for a
 *    one-step static helper.
 *
 * @note Requires @c CONFIG_OVE_INFER.
 * @{
 */

#ifndef OVE_INFER_H
#define OVE_INFER_H

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "ove/storage.h"

/**
 * @brief Tensor element types.
 *
 * Subset of TFLite tensor types that are relevant for microcontroller
 * inference (quantised int8/int16 and float32).
 */
enum ove_tensor_type {
	OVE_TENSOR_FLOAT32 = 0,
	OVE_TENSOR_INT8 = 1,
	OVE_TENSOR_UINT8 = 2,
	OVE_TENSOR_INT16 = 3,
	OVE_TENSOR_INT32 = 4,
};

/**
 * @brief Tensor descriptor returned by ove_model_input() / ove_model_output().
 *
 * Provides direct access to tensor data inside the arena, along with
 * shape and type metadata.  The @c data pointer is valid for the
 * lifetime of the model session.
 */
struct ove_tensor_info {
	void *data;		   /**< Pointer into the tensor arena buffer. */
	size_t size;		   /**< Total size of tensor data in bytes. */
	enum ove_tensor_type type; /**< Element type. */
	unsigned int ndims;	   /**< Number of dimensions. */
	int dims[5];		   /**< Shape, e.g. {1, 96, 96, 1}. */
};

/**
 * @brief Configuration for an ML inference session.
 *
 * @c model_data must point to a valid @c .tflite FlatBuffer.  It is
 * typically embedded as a @c const C array compiled into flash.
 * @c arena_size controls how much memory is reserved for intermediate
 * tensors; the actual requirement depends on the model.
 *
 * @warning The interpreter holds @c model_data by pointer for the entire
 *          lifetime of the model session. The caller MUST ensure the
 *          buffer remains valid and immutable from the successful
 *          @c ove_model_init() / @c ove_model_create() call until
 *          @c ove_model_deinit() / @c ove_model_destroy(). Passing a
 *          stack-allocated or transient heap buffer will result in
 *          silent memory corruption during @c ove_model_invoke().
 */
struct ove_model_config {
	const void *model_data; /**< Pointer to .tflite FlatBuffer data (must outlive the session). */
	size_t model_size;	/**< Size of model_data in bytes. */
	size_t arena_size;	/**< Tensor arena size in bytes. */
};

#ifdef CONFIG_OVE_INFER

/**
 * @brief Initialise a model using caller-supplied storage and arena.
 *
 * No heap allocation is performed.  The @p arena must be at least
 * @p cfg->arena_size bytes and remain valid for the lifetime of the model.
 * It should be 16-byte aligned for optimal CMSIS-NN performance.
 *
 * @param[out] model    Receives the opaque model handle on success.
 * @param[in]  storage  Pointer to caller-allocated backend storage.
 * @param[in]  arena    Caller-allocated tensor arena buffer.
 * @param[in]  cfg      Model configuration.
 * @return OVE_OK on success, OVE_ERR_INVALID_PARAM for bad arguments,
 *         OVE_ERR_ML_FAILED if model parsing or tensor allocation fails.
 *
 * @see ove_model_deinit, ove_model_create
 */
int ove_model_init(ove_model_t *model, ove_model_storage_t *storage, void *arena,
		   const struct ove_model_config *cfg);

/**
 * @brief Release resources held by a model initialised with ove_model_init().
 *
 * The static storage and arena buffer supplied at init time are not freed.
 *
 * @param[in] model  Handle returned by ove_model_init().
 *
 * @see ove_model_init
 */
void ove_model_deinit(ove_model_t model);

/* _create / _destroy — heap-gated */
#ifdef OVE_HEAP_INFER

/**
 * @brief Allocate and initialise a model from the heap.
 *
 * Both the backend storage and the tensor arena are allocated from
 * the heap.  The arena size is taken from @p cfg->arena_size.
 *
 * @param[out] model  Receives the opaque model handle on success.
 * @param[in]  cfg    Model configuration.
 * @return OVE_OK on success, OVE_ERR_NO_MEMORY if allocation fails,
 *         OVE_ERR_ML_FAILED if model parsing or tensor allocation fails.
 *
 * @see ove_model_destroy, ove_model_init
 */
int ove_model_create(ove_model_t *model, const struct ove_model_config *cfg);

/**
 * @brief Destroy and free a model allocated with ove_model_create().
 *
 * @param[in] model  Handle returned by ove_model_create().
 *
 * @see ove_model_create
 */
void ove_model_destroy(ove_model_t model);

#endif /* OVE_HEAP_INFER */

/**
 * @brief Run inference on the currently populated input tensor(s).
 *
 * Before calling this function, populate the input tensor data via
 * the pointer returned by ove_model_input().
 *
 * @param[in] model  Model handle.
 * @return OVE_OK on success, OVE_ERR_ML_FAILED on inference error.
 *
 * @see ove_model_input, ove_model_output
 */
int ove_model_invoke(ove_model_t model);

/**
 * @brief Get a descriptor for an input tensor.
 *
 * Populates @p info with the data pointer, shape, type, and size
 * of the input tensor at @p index.  Write input data to @p info->data
 * before calling ove_model_invoke().
 *
 * @param[in]  model  Model handle.
 * @param[in]  index  Zero-based input tensor index.
 * @param[out] info   Receives the tensor descriptor.
 * @return OVE_OK on success, OVE_ERR_INVALID_PARAM if index is out of range.
 *
 * @see ove_model_output, ove_model_invoke
 */
int ove_model_input(ove_model_t model, unsigned int index, struct ove_tensor_info *info);

/**
 * @brief Get a descriptor for an output tensor.
 *
 * Populates @p info with the data pointer, shape, type, and size
 * of the output tensor at @p index.  Call after ove_model_invoke()
 * to read results.
 *
 * @param[in]  model  Model handle.
 * @param[in]  index  Zero-based output tensor index.
 * @param[out] info   Receives the tensor descriptor.
 * @return OVE_OK on success, OVE_ERR_INVALID_PARAM if index is out of range.
 *
 * @see ove_model_input, ove_model_invoke
 */
int ove_model_output(ove_model_t model, unsigned int index, struct ove_tensor_info *info);

/**
 * @brief Return inference time of the last ove_model_invoke() in microseconds.
 *
 * The timing is measured using ove_time_get_us() around the interpreter
 * invocation.  Returns 0 if no inference has been run yet.
 *
 * @param[in] model  Model handle.
 * @return Inference duration in microseconds.
 */
uint64_t ove_model_last_inference_us(ove_model_t model);

#else /* !CONFIG_OVE_INFER */

/** @cond INTERNAL */
/* No _init/_deinit stubs here: OVE_MODEL_DEFINE_STATIC is itself gated by
 * #ifdef CONFIG_OVE_INFER in storage.h, so callers can't reach _init when
 * the subsystem is off — and ove_model_storage_t isn't declared either. */
static inline int ove_model_create(ove_model_t *m, const struct ove_model_config *c)
{
	(void)m;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_model_destroy(ove_model_t m)
{
	(void)m;
}
static inline int ove_model_invoke(ove_model_t m)
{
	(void)m;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_model_input(ove_model_t m, unsigned int i, struct ove_tensor_info *t)
{
	(void)m;
	(void)i;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_model_output(ove_model_t m, unsigned int i, struct ove_tensor_info *t)
{
	(void)m;
	(void)i;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline uint64_t ove_model_last_inference_us(ove_model_t m)
{
	(void)m;
	return 0;
}
/** @endcond */

#endif /* CONFIG_OVE_INFER */

#ifdef __cplusplus
}
#endif

#endif /* OVE_INFER_H */

/** @} */
