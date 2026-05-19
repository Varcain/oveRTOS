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

#ifndef CONFIG_OVE_ZERO_HEAP
#include <memory> /* std::unique_ptr in the capturing trampoline */
#endif

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
 * ove::Thread<4096> th(
 *     [](ove::stop_token tok) {
 *         while (!tok.stop_requested()) {
 *             do_work();
 *         }
 *     },
 *     OVE_PRIO_NORMAL, "worker");
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

	/**
	 * @brief Convertible to `bool` reflecting @ref stop_requested.
	 *
	 * Enables `if (tok) { … }` shorthand.  `explicit` prevents accidental
	 * implicit conversions in boolean-context-free positions.
	 */
	explicit operator bool() const noexcept
	{
		return stop_requested();
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
 * @class stop_source
 * @brief Writable counterpart to @ref stop_token.  `std::stop_source` analog.
 *
 * Use to issue cooperative stop requests on a thread without holding
 * the owning @ref Thread wrapper.  Pair with @ref stop_token for
 * read-only observers; together they mirror `std::stop_source` /
 * `std::stop_token` from C++20.
 *
 * Default-constructed is empty — equivalent to
 * `std::stop_source(std::nostopstate)`.  Bind to a thread either via
 * @ref Thread::get_stop_source or by explicit construction from an
 * `ove_thread_t` handle (advanced use).
 *
 * @code
 * ove::Thread<4096> worker(cooperative_worker, OVE_PRIO_NORMAL, "w");
 * ove::stop_source src = worker.get_stop_source();
 * // Pass `src` to helpers that need write-only stop capability.
 * src.request_stop();    // returns true the first time, false after.
 * @endcode
 *
 * Lifetime: the underlying stop state lives in the kernel thread's
 * TCB.  A stop_source remains usable until that thread terminates;
 * use after that is undefined (matches `std::stop_source` after the
 * associated state is destroyed).
 *
 * Trivially copyable; pass by value freely.  Distinct from
 * `std::stop_source` in that there is no shared reference count —
 * the kernel owns the single stop slot per thread.
 */
class stop_source
{
      public:
	constexpr stop_source() noexcept = default;
	constexpr explicit stop_source(ove_thread_t h) noexcept : handle_(h)
	{
	}

	/**
	 * @brief Set the stop flag on the associated thread.
	 * @return @c true if this call set the flag (it was previously
	 * unset AND a stop state exists).  Returns @c false on a
	 * default-constructed source or on repeat calls.  Matches
	 * @c std::stop_source::request_stop semantics.
	 *
	 * Safe from any context (ISR, other thread, the thread itself).
	 */
	bool request_stop() noexcept
	{
		if (handle_ == nullptr)
			return false;
		const bool was_stopped = ove_thread_should_stop(handle_);
		if (!was_stopped)
			ove_thread_request_stop(handle_);
		return !was_stopped;
	}

	/** @return @c true if the stop flag is set on the associated thread. */
	[[nodiscard]] bool stop_requested() const noexcept
	{
		return handle_ != nullptr && ove_thread_should_stop(handle_);
	}

	/** @return @c true if this source references a real thread (not default-constructed). */
	[[nodiscard]] bool stop_possible() const noexcept
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Convertible to `bool` reflecting @ref stop_requested.
	 * See @ref stop_token::operator bool.
	 */
	explicit operator bool() const noexcept
	{
		return stop_requested();
	}

	/** @return A @ref stop_token observing the same stop state. */
	[[nodiscard]] stop_token get_token() const noexcept
	{
		return stop_token{handle_};
	}

	/** @return The raw `ove_thread_t` handle (may be null). */
	[[nodiscard]] ove_thread_t handle() const noexcept
	{
		return handle_;
	}

	friend constexpr bool operator==(const stop_source &,
					 const stop_source &) noexcept = default;

      private:
	ove_thread_t handle_ = nullptr;
};

/**
 * @concept CooperativeThreadEntry
 * @brief Stateless callable invocable as `void(stop_token)`.
 *
 * The (captureless) cooperative `Thread` constructor accepts function
 * pointers and stateless lambdas matching this concept.  Capturing
 * lambdas are matched by @ref CapturingCooperativeEntry instead
 * (heap-mode only).
 */
template <typename F>
concept CooperativeThreadEntry = std::convertible_to<F, void (*)(stop_token)>;

#ifndef CONFIG_OVE_ZERO_HEAP
/**
 * @concept CapturingCooperativeEntry
 * @brief Capturing callable invocable as `void(stop_token)`.
 *
 * Picks up lambdas with captures, `std::function`-like wrappers,
 * functors with member state — anything that satisfies
 * `std::invocable<F&, stop_token>` but does NOT decay to a plain
 * function pointer (captureless lambdas / `void(*)(stop_token)` are
 * matched by @ref CooperativeThreadEntry).
 *
 * Heap-mode only: the matching `Thread` constructor heap-allocates a
 * box for the closure and lets the trampoline reclaim it on entry exit.
 * Not defined under `CONFIG_OVE_ZERO_HEAP`.
 */
template <typename F>
concept CapturingCooperativeEntry =
	std::invocable<F &, stop_token> && !std::convertible_to<F, void (*)(stop_token)>;
#endif

/**
 * @class thread_id
 * @brief Opaque identity for an oveRTOS thread.  `std::thread::id` analog.
 *
 * Default-constructible (represents "no thread"), equality-comparable,
 * and strict-weak-ordered (`<=>` via the underlying handle pointer).
 * Useful for storing thread identities in associative containers.
 *
 * Also exposed as @c Thread<N>::id (alias) to mirror @c std::thread::id
 * usage — `Thread<4096>::id` and `Thread<2048>::id` are the same type,
 * so values from differently-sized wrappers compare cleanly.
 *
 * @code
 * ove::Thread<4096> th(worker, OVE_PRIO_NORMAL, "w");
 * auto tid = th.get_id();
 * std::map<ove::thread_id, const char *> names;
 * names[tid] = "worker";
 * @endcode
 *
 * Note: like @c std::thread::id, the identity is only meaningful while
 * the thread is alive.  Once the kernel reaps the thread, the handle
 * value may be reused by a future thread and the two would compare
 * equal — same semantics as @c std::thread::id.
 */
class thread_id
{
      public:
	constexpr thread_id() noexcept = default;
	constexpr explicit thread_id(ove_thread_t h) noexcept : handle_(h)
	{
	}

	friend constexpr bool operator==(const thread_id &, const thread_id &) noexcept = default;
	friend constexpr auto operator<=>(const thread_id &, const thread_id &) noexcept = default;

	/** @return The raw `ove_thread_t` handle (may be null for default-constructed id). */
	[[nodiscard]] ove_thread_t native_handle() const noexcept
	{
		return handle_;
	}

      private:
	ove_thread_t handle_ = nullptr;
};

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

#ifndef CONFIG_OVE_ZERO_HEAP
/* Trampoline for the heap-mode capturing cooperative constructor.
 *
 * `ctx` is a `new`-allocated closure of type F that the constructor
 * minted; the trampoline reclaims it via std::unique_ptr so the box
 * is delete'd after the entry returns (or if the entry throws,
 * though our binding requires noexcept user code in practice).
 *
 * One template instantiation per F — the closure's exact type is
 * preserved, no type erasure or virtual dispatch.  Each instantiation
 * is small (typically 1-2 cache lines after LLVM inlining), so the
 * code-size cost scales with the number of distinct capturing
 * lambdas, not with the number of spawn sites. */
template <typename F> inline void capturing_trampoline(void *ctx)
{
	std::unique_ptr<F> f{static_cast<F *>(ctx)};
	ove_thread_t self;
	while ((self = ove_thread_get_self()) == nullptr) {
		ove_thread_yield();
	}
	(*f)(stop_token{self});
}
#endif

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
	/** @brief Opaque thread identity — alias of @ref thread_id.
	 *  Matches @c std::thread::id naming. */
	using id = thread_id;

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
	 * ove::Thread<4096> th(
	 *     [](ove::stop_token tok) {
	 *         while (!tok.stop_requested()) {
	 *             do_work();
	 *         }
	 *     },
	 *     OVE_PRIO_NORMAL, "worker");
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
		int err = ove_thread_init(&handle_, &storage_, name, &detail::stop_token_trampoline,
					  ctx, prio, StackSize, stack_);
#else
		int err = ove_thread_create(&handle_, name, &detail::stop_token_trampoline, ctx,
					    prio, StackSize);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

#ifndef CONFIG_OVE_ZERO_HEAP
	/**
	 * @brief Cooperative-cancellation constructor for **capturing** entries.
	 * Heap-mode only.  `std::jthread`-shaped.
	 *
	 * Accepts any callable invocable as `void(stop_token)` that isn't a
	 * plain function pointer — lambdas with captures, `std::function`,
	 * functors with member state.  The closure is heap-allocated into a
	 * box owned by the kernel thread; the trampoline reclaims (and
	 * deletes) it after the entry returns.
	 *
	 * @code
	 * int channel_id = 7;
	 * ove::Queue<Msg, 32> &q = get_queue();
	 *
	 * ove::Thread<4096> th(
	 *     [&](ove::stop_token tok) {
	 *         while (!tok.stop_requested()) {
	 *             q.send(Msg{channel_id});
	 *         }
	 *     },
	 *     OVE_PRIO_NORMAL, "w");
	 * @endcode
	 *
	 * **Lifetime**: captured references must outlive the worker — the
	 * destructor's `request_stop + join` guarantees that, provided the
	 * captured data outlives the @c Thread itself.  Same rule as
	 * @c std::jthread.
	 *
	 * **Cost**: one heap allocation per spawn (size = sizeof(F)).  One
	 * template instantiation per distinct closure type @p F — no type
	 * erasure or virtual dispatch.
	 *
	 * **Zero-heap**: this constructor is not defined.  Use the
	 * captureless cooperative constructor + a static context struct,
	 * or the legacy `void(void*)` constructor with a hand-rolled
	 * pointer.
	 */
	template <typename F>
	Thread(F &&entry, ove_prio_t prio, const char *name)
		requires(StackSize > 0) && CapturingCooperativeEntry<std::decay_t<F>>
	{
		using FT = std::decay_t<F>;
		FT *boxed = new FT(std::forward<F>(entry));
		int err = ove_thread_create(&handle_, name, &detail::capturing_trampoline<FT>,
					    boxed, prio, StackSize);
		if (err != OVE_OK) {
			delete boxed; /* trampoline never runs; reclaim the box */
			OVE_STATIC_INIT_ASSERT(err == OVE_OK);
		}
	}
#endif

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
	 * @brief Block the caller until the worker thread exits, then release
	 * the kernel handle.  Analog of @c std::thread::join.
	 *
	 * After @c join the wrapper is empty: @ref valid returns @c false and
	 * @ref ~Thread is a no-op.  Further method calls on the wrapper
	 * (other than @ref valid / @ref handle) are undefined behaviour —
	 * matches @c std::thread::join post-conditions.
	 *
	 * For cooperative workers, call @ref request_stop before @c join if
	 * the worker's loop terminates on the stop flag.  Workers without
	 * cooperative logic must terminate on their own; otherwise @c join
	 * blocks indefinitely.  Calling @c join from the worker thread
	 * itself deadlocks.
	 *
	 * Safe to call after @ref detach (no-op, since the handle is already
	 * null).
	 *
	 * Works in both heap and zero-heap modes — internally calls
	 * @c ove_thread_destroy (heap) or @c ove_thread_deinit (zero-heap),
	 * both of which wait for the worker to exit.
	 */
	void join() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_thread_deinit(handle_);
#else
		ove_thread_destroy(handle_);
#endif
		handle_ = nullptr;
	}

#ifdef CONFIG_OVE_ZERO_HEAP
	/**
	 * @brief Detach is **deleted** in zero-heap mode.
	 *
	 * Detach would skip the destructor's join wait, but the @c Thread
	 * wrapper owns the stack and storage that the kernel thread reads
	 * from.  Dropping the wrapper while the thread is still running is
	 * use-after-free.  Use @ref join (which waits) instead, or rely on
	 * the destructor to fire.  For program-lifetime threads, declare
	 * the wrapper as @c static so the destructor effectively never
	 * runs — that is the only safe "detach" in zero-heap mode.
	 */
	void detach() = delete;
#else
	/**
	 * @brief Release ownership of the kernel thread without waiting.
	 * Analog of @c std::thread::detach.
	 *
	 * After @c detach the wrapper is empty: @ref valid returns @c false
	 * and @ref ~Thread is a no-op.  The kernel thread keeps running
	 * with its own resources; the RTOS reaps them when the entry
	 * function returns.
	 *
	 * Unlike @ref ~Thread, @c detach does NOT call @ref request_stop —
	 * the worker is genuinely fire-and-forget.  Hand it out a
	 * @ref stop_token via @ref get_stop_token before detaching if you
	 * still want a way to cooperatively shut it down later.
	 *
	 * @code
	 * ove::Thread<4096>(worker, OVE_PRIO_NORMAL, "bg").detach();
	 * @endcode
	 *
	 * Heap-mode only.  See the zero-heap @ref detach stub for why.
	 */
	void detach() noexcept
	{
		handle_ = nullptr;
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
	 * @return On success, the populated @c ove_thread_stats.  On
	 *         failure, an `unexpected` @ref Error
	 *         (`Error::NotSupported` if the substrate doesn't track
	 *         stats).
	 */
	[[nodiscard]] Result<struct ove_thread_stats> get_runtime_stats() const noexcept
	{
		struct ove_thread_stats stats {
		};
		const int rc = ove_thread_get_runtime_stats(handle_, &stats);
		return from_rc(rc, stats);
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

	/**
	 * @brief Get a writable @ref stop_source for this thread.  Analog of
	 * @c std::jthread::get_stop_source.
	 *
	 * The returned source can issue stop requests without giving away
	 * the owning @c Thread.  Combine with @ref get_stop_token to hand
	 * out read-only observers separately from writable signal points.
	 *
	 * @code
	 * void register_shutdown_hook(ove::stop_source ss);
	 * register_shutdown_hook(worker.get_stop_source());
	 * @endcode
	 */
	[[nodiscard]] stop_source get_stop_source() const noexcept
	{
		return stop_source{handle_};
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
	 * @brief @c std::thread::joinable analog — `true` if this wrapper
	 * is associated with an active kernel thread.
	 *
	 * Returns @c false after @ref join or @ref detach (or default-
	 * construction failure, though our constructors abort instead).
	 * Synonym of @ref valid; both stay supported.
	 */
	[[nodiscard]] bool joinable() const noexcept
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Get the @ref id for this thread.  @c std::thread::get_id analog.
	 *
	 * Returns a default-constructed @ref id (compare-equal to @c id{})
	 * if the wrapper has been joined / detached / never spawned.
	 */
	[[nodiscard]] id get_id() const noexcept
	{
		return id{handle_};
	}

	/**
	 * @brief Returns the raw oveRTOS thread handle.
	 * @return The opaque `ove_thread_t` handle.
	 */
	ove_thread_t handle() const
	{
		return handle_;
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
 * @return On success, the populated @ref MemStats.  On failure, an
 *         `unexpected` @ref Error.
 */
[[nodiscard]] inline Result<MemStats> get_mem_stats() noexcept
{
	MemStats stats{};
	struct ove_mem_stats ms {
	};
	const int rc = ove_sys_get_mem_stats(&ms);
	if (rc == OVE_OK) {
		stats.total = ms.total;
		stats.free = ms.free;
		stats.used = ms.used;
		stats.peak_used = ms.peak_used;
	}
	return from_rc(rc, stats);
}

/* ── Thread enumeration ───────────────────────────────────────── */

/**
 * @brief Snapshot of a single thread.
 *
 * Alias of the C @ref ove_thread_info struct.  Exposes the full set of
 * fields the substrate provides (name, state, priority, stack_used,
 * stack_size, cpu_percent_x100, state_times), unchanged from the C
 * layer.  Default-initialise with `ThreadInfo info{};` for zero-init.
 *
 * Previous versions of the binding defined a C++-only subset struct
 * here and did a per-field copy in @ref thread_list.  That added a
 * silent 16-entry truncation cap (the binding's temp buffer was
 * fixed-size).  Aliasing the C struct lets @ref thread_list pass the
 * caller's array straight to the substrate — no temp buffer, no copy,
 * no binding-side cap.
 */
using ThreadInfo = struct ove_thread_info;

/**
 * @brief List all threads in the system.
 *
 * Fills @p out with up to @p max @ref ThreadInfo entries.
 *
 * @note The substrate caps the result at @c OVE_THREAD_LIST_MAX (16 in
 * the current implementation; tracked in `c-substrate-findings.md`
 * P2-2 for promotion to a public constant).  If @p max exceeds the
 * substrate's cap, the cap wins and the returned count reflects the
 * substrate limit, not @p max.
 *
 * @param[out] out Array to fill with thread info.
 * @param[in]  max Maximum entries the @p out buffer can hold.
 * @return On success, the actual number of entries written into @p out.
 *         On failure, an `unexpected` @ref Error.
 */
[[nodiscard]] inline Result<size_t> thread_list(ThreadInfo *out, size_t max) noexcept
{
	size_t n = 0;
	const int rc = ove_thread_list(out, max, &n);
	return from_rc(rc, n);
}

} /* namespace ove */

/**
 * @namespace ove::this_thread
 * @brief Free functions that operate on the currently running thread.
 *
 * Mirrors `std::this_thread::`. Use these for sleep, yield, and self-identification
 * from inside a thread entry function. Previously these lived as static members on
 * `Thread<>` (e.g. `Thread<>::sleep_ms(500)`), which read as "an operation on a
 * thread" when semantically it's "the current thread."
 */
namespace ove::this_thread
{

/**
 * @brief Suspend the calling thread for at least @p ms milliseconds.
 *        Plain-millisecond analog of `std::this_thread::sleep_for`.
 * @param[in] ms Sleep duration in milliseconds. Pass `0` to yield for one scheduler tick.
 */
inline void sleep_ms(uint32_t ms) noexcept
{
	ove_thread_sleep_ms(ms);
}

/**
 * @brief Suspend the calling thread for the given chrono duration.
 *        @c std::this_thread::sleep_for analog.
 * @tparam Rep    Underlying arithmetic type of @p d.
 * @tparam Period Tick period of @p d.
 * @param[in] d   Sleep duration; rounded down to whole milliseconds (the
 *                substrate's resolution).
 */
template <typename Rep, typename Period>
inline void sleep_for(std::chrono::duration<Rep, Period> d) noexcept
{
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
	ove_thread_sleep_ms(ms > 0 ? static_cast<uint32_t>(ms) : 0u);
}

/**
 * @brief Voluntarily relinquish the CPU to another ready thread of equal or higher
 *        priority. @c std::this_thread::yield analog.
 */
inline void yield() noexcept
{
	ove_thread_yield();
}

/** @return Raw C handle of the currently executing thread. */
inline ove_thread_t self() noexcept
{
	return ove_thread_get_self();
}

/** @return @ref thread_id of the currently executing thread. */
inline thread_id get_id() noexcept
{
	return thread_id{ove_thread_get_self()};
}

} /* namespace ove::this_thread */
