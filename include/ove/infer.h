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
 * across every oveRTOS backend (FreeRTOS, Zephyr, NuttX).
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
 *
 * The backend admits one live session across all handles. Constructors return
 * @c OVE_ERR_BUSY before allocation or writes when that session is occupied,
 * including during setup, failure cleanup and final teardown/free. A lost or
 * forgotten session retains admission indefinitely; there is no automatic
 * recovery. Operations on the admitted handle require external serialization.
 * Model bytes, caller storage and arena must remain valid through the final
 * close call returning.
 *
 * @par Trusted model contract
 * Models are trusted build-time assets containing valid, supported TFLite
 * programs. Model bytes must be 16-byte aligned and remain immutable throughout
 * the session. TFLM is compiled unmodified at the manifest revision. Alignment
 * and schema checks do not protect against malformed models or invalid numeric
 * input domains.
 *
 * @par Arena configuration
 * There are three arena regions:
 * - Below the backend startup precheck minimum: @c OVE_ERR_NO_MEMORY, with no
 *   SDK object constructed.
 * - Once persistent allocation and preparation requirements are met, an
 *   activation plan or planner bookkeeping that does not fit returns
 *   @c OVE_ERR_NO_MEMORY.
 * - Between the startup minimum and the model's persistent/preparation
 *   requirement: a configuration error with undefined behavior. Upstream
 *   kernels may use failed allocations; address zero can be mapped RAM on a
 *   target, so a null-based write need not fault.
 *
 * Measure a generously sized configuration on the actual target and kernel
 * provider with ove_model_arena_used(). Retain at least that usage plus 16 bytes
 * and qualify the configured arena. Remeasure after changing the model, provider
 * or TFLM revision. Final usage is not a universal preparation-peak bound;
 * shrinking until setup fails can enter the undefined region. Every admitted
 * setup clears the effective arena before SDK construction, costing O(arena_size).
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
 * Supported descriptor types: float32, int8, uint8, int16 and int32.
 * Tensor descriptor getters reject other TFLite element types.
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
 * Provides direct access to tensor data, along with shape and type metadata.
 * Outputs are read-only and can refer to immutable model bytes. The @c data
 * pointer is valid for the lifetime of the model session.
 * Successful descriptors report an exact supported type and rank from zero
 * (scalar) through five. Unused shape entries are zero.
 */
struct ove_tensor_info {
	void *data;		   /**< Tensor data; output access is read-only. */
	size_t size;		   /**< Total size of tensor data in bytes. */
	enum ove_tensor_type type; /**< Element type. */
	unsigned int ndims;	   /**< Number of dimensions, from zero through five. */
	int dims[5];		   /**< Exact shape; unused entries are zero. */
};

/**
 * @brief Configuration for an ML inference session.
 *
 * @c model_data must point to a trusted, valid @c .tflite FlatBuffer aligned to
 * 16 bytes, typically a generated @c const C array compiled into flash.
 * @c arena_size controls how much memory is reserved for intermediate
 * tensors; the actual requirement depends on the model.
 *
 * @warning The interpreter holds @c model_data by pointer for the entire
 *          lifetime of the model session. The caller MUST ensure the
 *          buffer remains valid and immutable from the successful
 *          @c ove_model_init() / @c ove_model_create() call until
 *          @c ove_model_deinit() / @c ove_model_destroy() returning. Passing a
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
 * Aligning the arena to 16 bytes avoids losing front padding. The arena must
 * satisfy the configuration contract above; an arbitrary nonzero size is not
 * sufficient. Effective arena bytes are cleared on each admitted setup.
 *
 * @param[out] model    Receives the opaque model handle on success.
 * @param[in]  storage  Pointer to caller-allocated backend storage.
 * @param[in]  arena    Caller-allocated tensor arena buffer.
 * @param[in]  cfg      Model configuration.
 * @return OVE_OK on success, OVE_ERR_INVALID_PARAM for bad arguments,
 *         OVE_ERR_NO_MEMORY for prechecked or allocator/planner-detected arena
 *         exhaustion, OVE_ERR_NOT_SUPPORTED for a schema mismatch,
 *         OVE_ERR_ML_FAILED for other setup failures, OVE_ERR_BUSY while another
 *         session owns the backend. Busy leaves the output handle, candidate
 *         storage and arena unchanged and performs no allocation.
 *
 * @see ove_model_deinit, ove_model_create
 */
OVE_NODISCARD int ove_model_init(ove_model_t *model, ove_model_storage_t *storage, void *arena,
				 const struct ove_model_config *cfg);

/**
 * @brief Release resources held by a model initialised with ove_model_init().
 *
 * The static storage and arena buffer supplied at init time are not freed.
 * Admission is released only after complete teardown. Calls on an admitted
 * live handle must be serialized; old handles cannot be used after its storage
 * is reinitialized for another session.
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
 * The trusted-model and three-region arena contracts above also apply.
 *
 * @param[out] model  Receives the opaque model handle on success.
 * @param[in]  cfg    Model configuration.
 * @return OVE_OK on success, OVE_ERR_INVALID_PARAM for bad arguments or model
 *         alignment, OVE_ERR_NO_MEMORY for owned allocation failure or detected
 *         arena exhaustion, OVE_ERR_NOT_SUPPORTED for a schema mismatch,
 *         OVE_ERR_ML_FAILED on other setup error, OVE_ERR_BUSY while another session
 *         owns the backend. Busy preserves the output and does not allocate.
 *
 * @see ove_model_destroy, ove_model_init
 */
OVE_NODISCARD int ove_model_create(ove_model_t *model, const struct ove_model_config *cfg);

/**
 * @brief Destroy and free a model allocated with ove_model_create().
 *
 * Admission remains held through final teardown and all owned frees. A model
 * over caller-supplied storage is deinitialized without freeing that storage.
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
OVE_NODISCARD int ove_model_invoke(ove_model_t model);

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
 * @return OVE_OK on success, OVE_ERR_NOT_SUPPORTED for an element type outside
 *         ove_tensor_type or rank above five, OVE_ERR_INVALID_PARAM for a null
 *         or accessible closed handle, null info or out-of-range index, and
 *         OVE_ERR_ML_FAILED if the interpreter cannot return the tensor.
 *         Errors leave info unchanged.
 *
 * @see ove_model_output, ove_model_invoke
 */
OVE_NODISCARD int ove_model_input(ove_model_t model, unsigned int index,
				  struct ove_tensor_info *info);

/**
 * @brief Get a descriptor for an output tensor.
 *
 * Populates @p info with the data pointer, shape, type, and size
 * of the output tensor at @p index.  Call after ove_model_invoke()
 * to read results. Output data is read-only, including model-backed constants.
 *
 * @param[in]  model  Model handle.
 * @param[in]  index  Zero-based output tensor index.
 * @param[out] info   Receives the tensor descriptor.
 * @return OVE_OK on success, OVE_ERR_NOT_SUPPORTED for an element type outside
 *         ove_tensor_type or rank above five, OVE_ERR_INVALID_PARAM for a null
 *         or accessible closed handle, null info or out-of-range index, and
 *         OVE_ERR_ML_FAILED if the interpreter cannot return the tensor.
 *         Errors leave info unchanged.
 *
 * @see ove_model_input, ove_model_invoke
 */
OVE_NODISCARD int ove_model_output(ove_model_t model, unsigned int index,
				   struct ove_tensor_info *info);

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

/**
 * @brief Report the live session's arena usage in bytes.
 *
 * Returns the interpreter's @c arena_used_bytes() plus the original arena's
 * front alignment padding. Measure on the target build and kernel provider;
 * the configured arena must retain at least this value plus 16 bytes. Keep the
 * qualified preparation capacity as well: final usage need not include peak
 * temporary planning/preparation space. Remeasure after model/provider/TFLM changes.
 *
 * @param[in] model Live session, or an accessible closed static handle.
 * @return Arena usage, or zero for a null or closed handle. A dangling handle
 *         remains invalid and must not be passed.
 */
size_t ove_model_arena_used(ove_model_t model);

#else /* !CONFIG_OVE_INFER */

/** @cond INTERNAL */
/* No _init/_deinit stubs here: OVE_MODEL_DEFINE_STATIC is itself gated by
 * #ifdef CONFIG_OVE_INFER in storage.h, so callers can't reach _init when
 * the subsystem is off — and ove_model_storage_t isn't declared either. */
OVE_NODISCARD static inline int ove_model_create(ove_model_t *m, const struct ove_model_config *c)
{
	(void)m;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_model_destroy(ove_model_t m)
{
	(void)m;
}
OVE_NODISCARD static inline int ove_model_invoke(ove_model_t m)
{
	(void)m;
	return OVE_ERR_NOT_SUPPORTED;
}
OVE_NODISCARD static inline int ove_model_input(ove_model_t m, unsigned int i,
						struct ove_tensor_info *t)
{
	(void)m;
	(void)i;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
OVE_NODISCARD static inline int ove_model_output(ove_model_t m, unsigned int i,
						 struct ove_tensor_info *t)
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
static inline size_t ove_model_arena_used(ove_model_t m)
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
