/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file thread.hpp
 * @brief Compile-time stack-sized thread with move semantics
 */

#pragma once

#include <ove/thread.h>
#include <ove/types.hpp>

namespace ove {

/**
 * @class Thread
 * @brief RAII wrapper around an oveRTOS thread (task).
 *
 * Creates and starts a thread on construction.  The thread is destroyed and
 * the underlying kernel resource is released on destruction.
 *
 * In zero-heap mode (`CONFIG_OVE_ZERO_HEAP`) the stack is stored as a
 * member array, so `StackSize` must be greater than zero and move operations
 * are disabled.  On heap-enabled builds a non-zero `StackSize` is still
 * required because it is passed to the kernel at construction time.
 *
 * @tparam StackSize Stack size in bytes for the thread (must be > 0).
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 */
template <size_t StackSize = 0>
class Thread {
public:
	/**
	 * @brief Constructs and starts the thread.
	 *
	 * Only participates in overload resolution when `StackSize > 0` and the
	 * entry function satisfies `ThreadEntry`.
	 *
	 * @tparam F       Entry function type satisfying `ThreadEntry`.
	 * @param[in] entry Function pointer (or compatible callable) to use as the
	 *                  thread entry point, with signature `void fn(void*)`.
	 * @param[in] ctx   Opaque pointer passed as the argument to `entry`.
	 * @param[in] prio  Thread priority as an `ove_prio_t` value.
	 * @param[in] name  Human-readable name for the thread (for debugging).
	 *
	 * Asserts at startup if thread creation fails.
	 */
	template <typename F>
	Thread(F entry, void *ctx, ove_prio_t prio, const char *name)
		requires (StackSize > 0) && ThreadEntry<F>
	{
		struct ove_thread_desc desc = {};
		desc.name = name;
		desc.entry = entry;
		desc.arg = ctx;
		desc.priority = prio;
		desc.stack_size = StackSize;
#ifdef CONFIG_OVE_ZERO_HEAP
		static_assert(StackSize > 0,
			      "StackSize must be > 0 in zero-heap mode");
		desc.stack = stack_;
		int err = ove_thread_init(&handle_, &storage_, &desc);
#else
		int err = ove_thread_create_(&handle_, &desc);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the thread wrapper, terminating and releasing the kernel thread.
	 */
	~Thread() {
		if (!handle_) return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_thread_deinit(handle_);
#else
		ove_thread_destroy(handle_);
#endif
	}

	Thread(const Thread &) = delete;
	Thread &operator=(const Thread &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Thread(Thread &&) = delete;
	Thread &operator=(Thread &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Thread(Thread &&other) noexcept : handle_(other.handle_) {
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Thread &operator=(Thread &&other) noexcept {
		if (this != &other) {
			if (handle_) ove_thread_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Changes the priority of the thread at runtime.
	 * @param[in] prio New priority value.
	 */
	void set_priority(ove_prio_t prio) {
		ove_thread_set_priority(handle_, prio);
	}

	/**
	 * @brief Suspends execution of the thread.
	 *
	 * The thread will not be scheduled until `resume()` is called.
	 */
	void suspend() {
		ove_thread_suspend(handle_);
	}

	/**
	 * @brief Resumes a previously suspended thread.
	 */
	void resume() {
		ove_thread_resume(handle_);
	}

	/**
	 * @brief Returns the current execution state of the thread.
	 * @return An `ove_thread_state_t` value representing the thread state.
	 */
	ove_thread_state_t get_state() const {
		return ove_thread_get_state(handle_);
	}

	/**
	 * @brief Returns the number of bytes used by the thread's stack so far.
	 * @return Peak stack usage in bytes.
	 */
	size_t get_stack_usage() const {
		return ove_thread_get_stack_usage(handle_);
	}

	/**
	 * @brief Retrieves runtime statistics for the thread.
	 * @param[out] stats Pointer to a struct that receives the statistics.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	int get_runtime_stats(struct ove_thread_stats *stats) const {
		return ove_thread_get_runtime_stats(handle_, stats);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the thread was successfully created.
	 */
	bool valid() const { return handle_ != nullptr; }

	/**
	 * @brief Returns the raw oveRTOS thread handle.
	 * @return The opaque `ove_thread_t` handle.
	 */
	ove_thread_t handle() const { return handle_; }

	/**
	 * @brief Suspends the calling thread for the specified duration.
	 * @param[in] ms Sleep duration in milliseconds.
	 */
	static void sleep_ms(uint32_t ms) {
		ove_thread_sleep_ms(ms);
	}

	/**
	 * @brief Yields the calling thread's remaining time slice to the scheduler.
	 */
	static void yield() {
		ove_thread_yield();
	}

	/**
	 * @brief Returns the oveRTOS handle of the currently executing thread.
	 * @return The opaque `ove_thread_t` handle of the calling thread.
	 */
	static ove_thread_t self() {
		return ove_thread_get_self();
	}

private:
	ove_thread_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_storage_t storage_ = {};
	OVE_THREAD_STACK_MEMBER_(stack_,
				     StackSize > 0 ? StackSize : 1);
#endif
};

} /* namespace ove */
