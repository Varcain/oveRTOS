/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#pragma once

#include <ove/infer.h>
#include <ove/storage.h>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_INFER

namespace ove
{

/**
 * @brief RAII wrapper for an ML inference model session.
 *
 * Wraps the C @c ove_model_* API with automatic resource management.
 * In zero-heap mode, storage and arena are embedded as class members.
 * In heap mode, they are allocated from the backend heap.
 *
 * @tparam ArenaSize  Tensor arena size in bytes (only used in zero-heap mode
 *                    for the embedded arena array; in heap mode this is
 *                    supplied via the config).
 */
template <size_t ArenaSize = 0> class Model
{
      public:
	/**
	 * @brief Construct a model from the given configuration.
	 *
	 * Loads the FlatBuffer model and allocates tensors.  Asserts on failure
	 * (embedded systems typically cannot recover from model load failure).
	 */
	explicit Model(const struct ove_model_config &cfg)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_model_init(&handle_, &storage_, arena_, &cfg);
#else
		int err = ove_model_create(&handle_, &cfg);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	~Model() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_model_deinit(handle_);
#else
		ove_model_destroy(handle_);
#endif
	}

	Model(const Model &) = delete;
	Model &operator=(const Model &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Model(Model &&) = delete;
	Model &operator=(Model &&) = delete;
#else
	/** @brief Move constructor — transfers handle ownership; source becomes empty. */
	Model(Model &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}
	/** @brief Move-assignment — destroys current model, then takes `other`'s handle. */
	Model &operator=(Model &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_model_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/** @brief Run the model forward pass.
	 *  @return Empty `Result<void>` on success; `unexpected`
	 *          @ref Error on failure.
	 */
	[[nodiscard]] Result<void> invoke() noexcept
	{
		return from_rc(ove_model_invoke(handle_));
	}

	/** @brief Get a typed pointer to input tensor data; nullptr on failure. */
	template <typename T> T *input_data(unsigned int index = 0)
	{
		struct ove_tensor_info info;
		if (ove_model_input(handle_, index, &info) != OVE_OK)
			return nullptr;
		return static_cast<T *>(info.data);
	}

	/** @brief Get a typed pointer to output tensor data (const); nullptr on failure. */
	template <typename T> const T *output_data(unsigned int index = 0) const
	{
		struct ove_tensor_info info;
		if (ove_model_output(handle_, index, &info) != OVE_OK)
			return nullptr;
		return static_cast<const T *>(info.data);
	}

	/** @brief Get full tensor descriptor for an input.
	 *  @return On success, the populated @c ove_tensor_info.  On
	 *          failure, an `unexpected` @ref Error.
	 */
	[[nodiscard]] Result<struct ove_tensor_info> input(unsigned int index) const noexcept
	{
		struct ove_tensor_info info{};
		const int rc = ove_model_input(handle_, index, &info);
		return from_rc(rc, info);
	}

	/** @brief Get full tensor descriptor for an output. */
	[[nodiscard]] Result<struct ove_tensor_info> output(unsigned int index) const noexcept
	{
		struct ove_tensor_info info{};
		const int rc = ove_model_output(handle_, index, &info);
		return from_rc(rc, info);
	}

	/** @brief Return last inference duration in microseconds. */
	uint64_t last_inference_us() const
	{
		return ove_model_last_inference_us(handle_);
	}

	/** @brief Access the underlying C handle. */
	ove_model_t handle() const
	{
		return handle_;
	}

      private:
	ove_model_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_model_storage_t storage_ = {};
	alignas(16) uint8_t arena_[ArenaSize] = {};
#endif
};

} // namespace ove

#endif /* CONFIG_OVE_INFER */
