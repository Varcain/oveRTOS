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

namespace ove
{

/* ── Cooperative cancellation token ─────────────────────────────────── */

/**
 * @class stop_token
 * @brief Lightweight read-only handle to a thread's cooperative cancellation flag.
 *
 * Mirrors `std::stop_token` (C++20) but does NOT pull in the heavyweight
 * `<stop_token>` header (which drags in `<stop_source>`, `<stop_callback>`,
 * and condvar machinery — overkill for embedded).  Trivially copyable;
 * pass by value freely.
 *
 * Workers built with the cooperative `Thread` constructor receive a
 * `stop_token` as their entry argument:
 * @code
 * ove::Thread<4096> th("worker", ove::Priority::Normal,
 *     [](ove::stop_token tok) {
 *         while (!tok.stop_requested()) {
 *             do_work();
 *         }
 *     });
 * @endcode
 *
 * The token can be passed to helper functions that need to bail out
 * cooperatively.  Setting the underlying flag is done via
 * `Thread::request_stop()` or the parent's `~Thread()`; the token is
 * read-only by design.
 *
 * @see Thread, ove_thread_request_stop, ove_thread_should_stop
 */
class stop_token
{
      public:
	constexpr stop_token() noexcept = default;
	constexpr explicit stop_token(ove_thread_t h) noexcept : handle_(h)
	{
	}

	/** @return `true` if a stop has been requested for the referenced thread. */
	[[nodiscard]] bool stop_requested() const noexcept
	{
		return handle_ != nullptr && ove_thread_should_stop(handle_);
	}

	/** @return `true` if the token references a real thread (non-default-constructed). */
	[[nodiscard]] bool stop_possible() const noexcept
	{
		return handle_ != nullptr;
	}

	/** @return The raw `ove_thread_t` handle for advanced use. */
	[[nodiscard]] ove_thread_t handle() const noexcept
	{
		return handle_;
	}

      private:
	ove_thread_t handle_ = nullptr;
};

/**
 * @concept CooperativeThreadEntry
 * @brief Stateless callable invocable as `void(stop_token)`.
 *
 * The cooperative `Thread` constructor accepts function pointers and
 * stateless lambdas matching this concept.  Capturing lambdas are
 * rejected because the binding cannot heap-allocate captures in
 * zero-heap mode.
 */
template <typename F>
concept CooperativeThreadEntry = std::convertible_to<F, void (*)(stop_token)>;

namespace detail
{

/* Trampoline bridging the substrate's `void(void*)` entry signature to
 * the cooperative `void(stop_token)` user signature.
 *
 * The user's function pointer is passed verbatim through `ctx` via
 * reinterpret_cast — guarded by a compile-time size check on every
 * supported target (function pointer and data pointer share width on
 * POSIX, ARM Cortex-M, and Xtensa).
 *
 * Token construction uses `ove_thread_get_self()` after a brief poll
 * loop: on some backends (notably FreeRTOS) the task-tag lookup that
 * powers `get_self` is populated by the spawning thread AFTER the new
 * thread starts running, so the first call may return NULL.  Yielding
 * lets the spawner finish the tag publish. */
inline void stop_token_trampoline(void *ctx)
{
	static_assert(sizeof(void *) == sizeof(void (*)(stop_token)),
		      "ove::stop_token trampoline requires function and data pointers "
		      "to share width — fix by adding a per-target trampoline shim");
	auto user_fn = reinterpret_cast<void (*)(stop_token)>(ctx);
	ove_thread_t self;
	while ((self = ove_thread_get_self()) == nullptr) {
		ove_thread_yield();
	}
	user_fn(stop_token{self});
}

} // namespace detail

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
template <size_t StackSize = 0> class Thread
{
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
		requires(StackSize > 0) && ThreadEntry<F>
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		static_assert(StackSize > 0, "StackSize must be > 0 in zero-heap mode");
		int err = ove_thread_init(&handle_, &storage_, name, entry, ctx, prio, StackSize,
					  stack_);
#else
		int err = ove_thread_create(&handle_, name, entry, ctx, prio, StackSize);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Cooperative-cancellation constructor — `std::jthread` analog.
	 *
	 * The entry function receives an @ref ove::stop_token and must poll
	 * @c stop_token::stop_requested() in any long-running loop so that
	 * @ref ~Thread (or an explicit @ref request_stop) can drive it to a
	 * clean exit instead of deadlocking the join wait.
	 *
	 * @code
	 * ove::Thread<4096> th("worker", ove::Priority::Normal,
	 *     [](ove::stop_token tok) {
	 *         while (!tok.stop_requested()) {
	 *             do_work();
	 *         }
	 *     });
	 * @endcode
	 *
	 * The entry must be a stateless callable (function pointer or
	 * captureless lambda) — the binding has no heap to store captures
	 * in zero-heap mode.  Use the @c void(void*) constructor above with
	 * a static context struct if you need bound data.
	 *
	 * Only participates in overload resolution when @c StackSize > 0
	 * and @p F satisfies @ref CooperativeThreadEntry.
	 */
	template <typename F>
	Thread(F entry, ove_prio_t prio, const char *name)
		requires(StackSize > 0) && CooperativeThreadEntry<F>
	{
		void (*fn)(stop_token) = entry;
		void *ctx = reinterpret_cast<void *>(fn);
#ifdef CONFIG_OVE_ZERO_HEAP
		static_assert(StackSize > 0, "StackSize must be > 0 in zero-heap mode");
		int err = ove_thread_init(&handle_, &storage_, name,
					  &detail::stop_token_trampoline, ctx, prio, StackSize,
					  stack_);
#else
		int err = ove_thread_create(&handle_, name, &detail::stop_token_trampoline, ctx,
					    prio, StackSize);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the thread wrapper, terminating and releasing the kernel thread.
	 *
	 * Calls @ref ove_thread_request_stop before the join wait so
	 * cooperative workers (built with the @ref stop_token constructor)
	 * exit cleanly without blocking the destructor.  Non-cooperative
	 * workers (built with the legacy @c void(void*) constructor) are
	 * unaffected — the destructor still blocks until the entry function
	 * returns by its own logic.
	 */
	~Thread() noexcept
	{
		if (!handle_)
			return;
		ove_thread_request_stop(handle_);
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
	Thread(Thread &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Thread &operator=(Thread &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_thread_destroy(handle_);
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
	void set_priority(ove_prio_t prio)
	{
		ove_thread_set_priority(handle_, prio);
	}

	/**
	 * @brief Suspends execution of the thread.
	 *
	 * The thread will not be scheduled until `resume()` is called.
	 */
	void suspend()
	{
		ove_thread_suspend(handle_);
	}

	/**
	 * @brief Resumes a previously suspended thread.
	 */
	void resume()
	{
		ove_thread_resume(handle_);
	}

	/**
	 * @brief Returns the current execution state of the thread.
	 * @return An `ove_thread_state_t` value representing the thread state.
	 */
	ove_thread_state_t get_state() const
	{
		return ove_thread_get_state(handle_);
	}

	/**
	 * @brief Returns the number of bytes used by the thread's stack so far.
	 * @return Peak stack usage in bytes.
	 */
	size_t get_stack_usage() const
	{
		return ove_thread_get_stack_usage(handle_);
	}

	/**
	 * @brief Retrieves runtime statistics for the thread.
	 * @param[out] stats Pointer to a struct that receives the statistics.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	int get_runtime_stats(struct ove_thread_stats *stats) const
	{
		return ove_thread_get_runtime_stats(handle_, stats);
	}

	/**
	 * @brief Requests the thread to stop cooperatively.
	 *
	 * Sets the thread's cancellation flag.  The worker must poll
	 * @ref stop_token::stop_requested via the @ref stop_token it
	 * received (cooperative-cancellation constructor) for this to have
	 * any effect — the substrate does NOT force-terminate.
	 *
	 * Safe from any context (ISR, other thread, the thread itself).
	 * Idempotent; the flag is sticky.
	 *
	 * @see ~Thread, stop_token
	 */
	void request_stop() noexcept
	{
		if (handle_)
			ove_thread_request_stop(handle_);
	}

	/**
	 * @brief Get a token that observes this thread's cancellation flag.
	 *
	 * Useful for passing the token to helper functions that need to
	 * bail out cooperatively without being given control of the
	 * `Thread` object itself.
	 */
	[[nodiscard]] stop_token get_stop_token() const noexcept
	{
		return stop_token{handle_};
	}

	/** @return `true` if @ref request_stop has been called on this thread. */
	[[nodiscard]] bool stop_requested() const noexcept
	{
		return handle_ != nullptr && ove_thread_should_stop(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the thread was successfully created.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS thread handle.
	 * @return The opaque `ove_thread_t` handle.
	 */
	ove_thread_t handle() const
	{
		return handle_;
	}

	/**
	 * @brief Suspends the calling thread for the specified duration.
	 * @param[in] ms Sleep duration in milliseconds.
	 */
	static void sleep_ms(uint32_t ms)
	{
		ove_thread_sleep_ms(ms);
	}

	/**
	 * @brief Yields the calling thread's remaining time slice to the scheduler.
	 */
	static void yield()
	{
		ove_thread_yield();
	}

	/**
	 * @brief Returns the oveRTOS handle of the currently executing thread.
	 * @return The opaque `ove_thread_t` handle of the calling thread.
	 */
	static ove_thread_t self()
	{
		return ove_thread_get_self();
	}

      private:
	ove_thread_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	/* `stack_` precedes `storage_` so the latter (whose underlying C
	 * struct ends in a flexible-array `stack[]`) sits at the end of the
	 * C++ class.  C99 allows FAMs anywhere in a struct, but C++ rejects
	 * another member after one — see
	 * backends/freertos/include/ove_storage_freertos.h FAM commentary. */
	OVE_THREAD_STACK_MEMBER_(stack_, StackSize > 0 ? StackSize : 1);
	ove_thread_storage_t storage_ = {};
#endif
};

/* ── System memory statistics ──────────────────────────────────── */

/**
 * @struct MemStats
 * @brief System heap statistics snapshot.
 */
struct MemStats {
	size_t total{};	    /**< Total heap size in bytes. */
	size_t free{};	    /**< Current free heap in bytes. */
	size_t used{};	    /**< Current used heap in bytes. */
	size_t peak_used{}; /**< High-water-mark usage in bytes. */
};

/**
 * @brief Query system heap statistics.
 * @param[out] stats Structure to fill.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int get_mem_stats(MemStats &stats)
{
	struct ove_mem_stats ms {
	};
	int ret = ove_sys_get_mem_stats(&ms);
	if (ret == OVE_OK) {
		stats.total = ms.total;
		stats.free = ms.free;
		stats.used = ms.used;
		stats.peak_used = ms.peak_used;
	}
	return ret;
}

/* ── Thread enumeration ───────────────────────────────────────── */

/**
 * @struct ThreadInfo
 * @brief Snapshot of a single thread.
 */
struct ThreadInfo {
	const char *name{};				    ///< Thread name.
	ove_thread_state_t state{OVE_THREAD_STATE_UNKNOWN}; ///< Current state.
	int priority{};					    ///< Scheduling priority.
	size_t stack_used{};				    ///< Peak stack usage in bytes.
};

/**
 * @brief List all threads in the system.
 * @param[out] out   Array to fill with thread info.
 * @param[in]  max   Maximum entries.
 * @param[out] count Actual count written (may be nullptr).
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int thread_list(ThreadInfo *out, size_t max, size_t *count = nullptr)
{
	struct ove_thread_info buf[16];
	size_t n = 0;
	int ret = ove_thread_list(buf, max < 16 ? max : 16, &n);
	if (ret == OVE_OK) {
		for (size_t i = 0; i < n && i < max; i++) {
			out[i].name = buf[i].name;
			out[i].state = buf[i].state;
			out[i].priority = buf[i].priority;
			out[i].stack_used = buf[i].stack_used;
		}
		if (count)
			*count = n;
	}
	return ret;
}

} /* namespace ove */
