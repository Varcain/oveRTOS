/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file infer.hpp
 * @brief C++ wrappers for ML inference — `ove::Model` over LiteRT
 *        (TensorFlow Lite Micro). Available when `CONFIG_OVE_INFER` is set.
 */

#pragma once

#include <ove/infer.h>
#include <ove/storage.h>
#include <ove/error.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef CONFIG_OVE_INFER

namespace ove
{

template <size_t ArenaSize> class ModelStorage;

namespace detail
{

/* Storage and arena a zero-heap Model<N> carries inside itself. */
template <size_t N> struct ModelEmbedded {
	ove_model_storage_t storage = {};
	alignas(16) uint8_t arena[N] = {};
};
struct ModelNothingEmbedded {
};

#ifdef CONFIG_OVE_ZERO_HEAP
inline constexpr bool model_zero_heap = true;
#else
inline constexpr bool model_zero_heap = false;
#endif

/**
 * @brief The session handle and its operations, common to every
 *        @ref ove::Model.
 */
class ModelBase
{
      public:
	ModelBase(const ModelBase &) = delete;
	ModelBase &operator=(const ModelBase &) = delete;

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
		struct ove_tensor_info info {
		};
		const int rc = ove_model_input(handle_, index, &info);
		return from_rc(rc, info);
	}

	/** @brief Get full tensor descriptor for an output. */
	[[nodiscard]] Result<struct ove_tensor_info> output(unsigned int index) const noexcept
	{
		struct ove_tensor_info info {
		};
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

      protected:
	ModelBase() noexcept = default;
	explicit ModelBase(ove_model_t handle) noexcept : handle_(handle)
	{
	}
	ModelBase(ModelBase &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}
	ModelBase &operator=(ModelBase &&other) noexcept
	{
		if (this != &other) {
			release();
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
	~ModelBase() noexcept
	{
		release();
	}

	ove_model_t handle_ = nullptr;

      private:
	/* ove_model_destroy also tears down a session over caller storage
	 * (it frees only what ove_model_create allocated); zero-heap builds
	 * have only ove_model_deinit. */
	void release() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_model_deinit(handle_);
#else
		ove_model_destroy(handle_);
#endif
		handle_ = nullptr;
	}
};

} // namespace detail

/**
 * @brief RAII wrapper for an ML inference model session.
 *
 * Wraps the C @c ove_model_* API with automatic resource management.  The
 * session lives until the object is destroyed.
 *
 * - Heap mode: the constructor allocates storage and arena from the backend
 *   heap; @p ArenaSize is unused.  Movable.
 * - Zero-heap mode with @p ArenaSize > 0: the storage and a @p ArenaSize
 *   arena are members, so the object is that large: wherever it lives, a
 *   thread's stack included, must have room for it.  Not movable.
 * - `Model<>` (@p ArenaSize 0) in zero-heap mode only comes from
 *   @ref ModelStorage::load, and borrows that storage.  Movable.
 *
 * For an arena of any size, prefer a static @ref ModelStorage and load
 * each model into it as needed: the session object is then just a handle.
 * One model runs at a time: the backend shares its interpreter between
 * sessions.
 *
 * @tparam ArenaSize  Tensor arena size in bytes embedded in zero-heap mode.
 */
template <size_t ArenaSize = 0> class Model : public detail::ModelBase
{
	static constexpr bool embedded = detail::model_zero_heap && ArenaSize > 0;

      public:
	/**
	 * @brief Construct a model from the given configuration.
	 *
	 * Loads the FlatBuffer model and allocates tensors.  Asserts on failure
	 * (embedded systems typically cannot recover from model load failure).
	 * Not available for a zero-heap `Model<>`: load it from a
	 * @ref ModelStorage.
	 */
	explicit Model(const struct ove_model_config &cfg)
		requires(!detail::model_zero_heap || ArenaSize > 0)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_model_init(&handle_, &mem_.storage, mem_.arena, &cfg);
#else
		int err = ove_model_create(&handle_, &cfg);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/** @brief Move constructor — transfers the session; source becomes empty. */
	Model(Model &&other) noexcept
		requires(!embedded)
	= default;
	/** @brief Move-assignment — ends this session, then takes `other`'s. */
	Model &operator=(Model &&other) noexcept
		requires(!embedded)
	= default;
	Model(Model &&)
		requires(embedded)
	= delete;
	Model &operator=(Model &&)
		requires(embedded)
	= delete;

	~Model() noexcept = default;

      private:
	template <size_t> friend class ModelStorage;

	explicit Model(ove_model_t handle) noexcept : detail::ModelBase(handle)
	{
	}

	[[no_unique_address]] std::conditional_t<embedded, detail::ModelEmbedded<ArenaSize>,
						 detail::ModelNothingEmbedded> mem_;
};

/**
 * @brief Storage and tensor arena for one model session at a time, reused
 *        across models — the C++ form of the C API's caller-supplied
 *        storage and arena.
 *
 * Meant to be static (it holds the whole arena): load a model into it with
 * load(), use the returned session, let the session go, then load the next
 * model.  The returned @ref Model borrows this storage: it must be destroyed
 * before the next load() and before the storage itself, and only one may
 * exist at a time.  Works in heap and zero-heap modes alike, and allocates
 * nothing.
 *
 * @code
 * static ove::ModelStorage<32768> storage;
 *
 * auto model = storage.load(model_data, model_size);
 * if (!model)
 *     return model.error();
 * model->input_data<int8_t>(0)[0] = 42;
 * if (auto r = model->invoke(); !r)
 *     return r.error();
 * @endcode
 *
 * @tparam ArenaSize  Tensor arena size in bytes.
 */
template <size_t ArenaSize> class ModelStorage
{
	static_assert(ArenaSize > 0, "ModelStorage needs an arena");

      public:
	constexpr ModelStorage() noexcept = default;
	ModelStorage(const ModelStorage &) = delete;
	ModelStorage &operator=(const ModelStorage &) = delete;

	/**
	 * @brief Load a model into this storage.
	 *
	 * @param[in] model_data  The .tflite FlatBuffer; must outlive the session.
	 * @param[in] model_size  Its size in bytes.
	 * @return The session, borrowing this storage; or an `unexpected`
	 *         @ref Error if the model does not load (Error::MlFailed
	 *         when it is not a valid model or the arena is too small).
	 */
	[[nodiscard]] Result<Model<>> load(const void *model_data, size_t model_size) noexcept
	{
		const struct ove_model_config cfg = {model_data, model_size, ArenaSize};
		ove_model_t handle = nullptr;
		const int rc = ove_model_init(&handle, &storage_, arena_, &cfg);
		if (rc != OVE_OK)
			return std::unexpected{static_cast<Error>(rc)};
		return Model<>(handle);
	}

      private:
	ove_model_storage_t storage_ = {};
	alignas(16) uint8_t arena_[ArenaSize] = {};
};

} // namespace ove

#endif /* CONFIG_OVE_INFER */
