/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file sync.hpp
 * @brief RAII wrappers for mutexes, semaphores, events, condition variables, and lock guards
 */

#pragma once

#include <ove/sync.h>
#include <ove/types.hpp>
#include <ove/error.hpp>
#include <atomic>

#ifdef CONFIG_OVE_SYNC

namespace ove
{

/**
 * @class Mutex
 * @brief RAII wrapper around an oveRTOS non-recursive mutex.
 *
 * Constructs the underlying kernel mutex object on creation and destroys it
 * on destruction.  With `CONFIG_OVE_ZERO_HEAP` the mutex storage is held
 * inline in the wrapper; move operations are therefore disabled in that
 * configuration because the kernel may hold a pointer to the internal buffer.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 * @note `lock()` is marked `[[nodiscard]]`; ignoring its return value risks
 *       deadlock.
 */
class Mutex
{
      public:
	/**
	 * @brief Constructs and initialises the mutex.
	 *
	 * Calls `ove_mutex_init` (zero-heap) or `ove_mutex_create` (heap).
	 * Asserts at startup if initialisation fails.
	 */
	Mutex()
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_mutex_init(&handle_, &storage_);
#else
		int err = ove_mutex_create(&handle_);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the mutex, releasing the underlying kernel resource.
	 *
	 * If the handle is null (e.g., after a move), the destructor is a no-op.
	 */
	~Mutex() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_mutex_deinit(handle_);
#else
		ove_mutex_destroy(handle_);
#endif
	}

	Mutex(const Mutex &) = delete;
	Mutex &operator=(const Mutex &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Mutex(Mutex &&) = delete;
	Mutex &operator=(Mutex &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source mutex; its handle is set to null after the move.
	 */
	Mutex(Mutex &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source mutex; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Mutex &operator=(Mutex &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_mutex_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Acquires the mutex, blocking indefinitely.  `std::mutex::lock` analog.
	 *
	 * Failure of an indefinite lock means the handle is unusable
	 * (moved-from, stale, or subsystem mid-deinit) — programming
	 * error, not a recoverable runtime condition.  Aborts via
	 * @c OVE_STATIC_INIT_ASSERT on non-OK return; mirrors how
	 * @ref Thread / @ref Queue constructors handle similar failures.
	 *
	 * Pairs with @ref try_lock to satisfy the @c Lockable named
	 * requirement, enabling @c std::lock_guard<ove::Mutex> and
	 * @c std::scoped_lock(m1, m2) composition.  Does **not** satisfy
	 * @c TimedLockable — @ref try_lock_for and @ref try_lock_until
	 * return `Result<void>` not `bool`, so `std::unique_lock<>` 's
	 * timeout overloads won't compile.  Use those methods directly
	 * and switch on the @ref Result instead.
	 */
	void lock()
	{
		const int err = ove_mutex_lock(handle_, OVE_WAIT_FOREVER);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Attempts to acquire the mutex without blocking.  `std::Lockable` requirement.
	 * @return `true` on acquisition, `false` if the mutex is held by another thread.
	 */
	[[nodiscard]] bool try_lock()
	{
		return ove_mutex_lock(handle_, 0) == OVE_OK;
	}

	/**
	 * @brief Attempts to acquire the mutex within @p rel.
	 *
	 * Templated over `std::chrono::duration` so any unit composes
	 * (`100ms`, `1s`, `2us` …).  Does **not** satisfy the
	 * `TimedLockable` named requirement — that requires a `bool`
	 * return; this returns `Result<void>` so timeout (`Error::Timeout`)
	 * and substrate errors are reported distinctly.  Consequence:
	 * `std::unique_lock<ove::Mutex>::try_lock_for` won't compile —
	 * use this method directly and switch on the @ref Result instead.
	 *
	 * @param[in] rel Relative timeout (any `std::chrono::duration` unit).
	 * @return Empty `Result<void>` on acquisition; `unexpected`
	 *         @ref Error::Timeout if the deadline elapsed without
	 *         the lock being acquired; `unexpected` with another
	 *         @ref Error value on backend failure.
	 */
	template <class Rep, class Period>
	[[nodiscard]] Result<void>
	try_lock_for(const std::chrono::duration<Rep, Period> &rel) noexcept
	{
		return from_rc(ove_mutex_lock(handle_, to_timeout_ns(rel)));
	}

	/**
	 * @brief Attempts to acquire the mutex by @p deadline.
	 *
	 * Templated over the clock so callers can pass
	 * `std::chrono::steady_clock::now() + 100ms` or
	 * `ove::steady_clock::now() + 100ms` interchangeably (the deadline
	 * is converted to a relative duration internally; the clock's
	 * epoch does not need to match @ref ove::steady_clock).
	 *
	 * @return As @ref try_lock_for — `Result<void>` with
	 *         `Error::Timeout` on timeout.
	 */
	template <class Clock, class Duration>
	[[nodiscard]] Result<void>
	try_lock_until(const std::chrono::time_point<Clock, Duration> &deadline) noexcept
	{
		const auto rel = deadline - Clock::now();
		return from_rc(ove_mutex_lock(handle_, to_timeout_ns(rel)));
	}

	/**
	 * @brief Releases the mutex.
	 *
	 * Must be called from the same thread context that acquired the lock.
	 */
	void unlock()
	{
		ove_mutex_unlock(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the mutex was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS mutex handle.
	 * @return The opaque `ove_mutex_t` handle.
	 */
	ove_mutex_t handle() const
	{
		return handle_;
	}

      private:
	ove_mutex_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mutex_storage_t storage_ = {};
#endif
};

/* Regression guard.  `BasicLockable` and `Lockable` are named
 * requirements (not concepts) in the C++ standard; define equivalent
 * local concepts and static_assert against them so a future change
 * that breaks std-composition (e.g. removing `try_lock`) trips the
 * build instead of failing far away in a user's `std::scoped_lock`
 * instantiation. */
namespace detail
{

template <class M>
concept basic_lockable = requires(M m) {
	{ m.lock() };
	{ m.unlock() };
};

template <class M>
concept lockable = basic_lockable<M> && requires(M m) {
	{ m.try_lock() } -> std::convertible_to<bool>;
};

} /* namespace detail */

static_assert(detail::basic_lockable<Mutex>,
	      "ove::Mutex must satisfy the BasicLockable named requirement");
static_assert(detail::lockable<Mutex>, "ove::Mutex must satisfy the Lockable named requirement");

/**
 * @class RecursiveMutex
 * @brief RAII wrapper around an oveRTOS recursive mutex.
 *
 * A recursive mutex may be locked multiple times by the same thread without
 * deadlocking; each `lock` call must be paired with a matching `unlock` call.
 * Ownership and zero-heap behaviour mirror that of `Mutex`.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 */
class RecursiveMutex
{
      public:
	/**
	 * @brief Constructs and initialises the recursive mutex.
	 *
	 * Calls `ove_recursive_mutex_init` (zero-heap) or
	 * `ove_recursive_mutex_create` (heap).  Asserts at startup on failure.
	 */
	RecursiveMutex()
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_recursive_mutex_init(&handle_, &storage_);
#else
		int err = ove_recursive_mutex_create(&handle_);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the recursive mutex, releasing the underlying kernel resource.
	 */
	~RecursiveMutex() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_mutex_deinit(handle_);
#else
		ove_recursive_mutex_destroy(handle_);
#endif
	}

	RecursiveMutex(const RecursiveMutex &) = delete;
	RecursiveMutex &operator=(const RecursiveMutex &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	RecursiveMutex(RecursiveMutex &&) = delete;
	RecursiveMutex &operator=(RecursiveMutex &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	RecursiveMutex(RecursiveMutex &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	RecursiveMutex &operator=(RecursiveMutex &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_recursive_mutex_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Acquires the recursive mutex, blocking indefinitely.
	 *
	 * Same fatal-on-failure semantics as @ref Mutex::lock.
	 */
	void lock()
	{
		const int err = ove_recursive_mutex_lock(handle_, OVE_WAIT_FOREVER);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Non-blocking acquisition attempt.  `std::Lockable` requirement.
	 */
	[[nodiscard]] bool try_lock()
	{
		return ove_recursive_mutex_lock(handle_, 0) == OVE_OK;
	}

	/**
	 * @brief Bounded-wait acquisition.  Same shape as
	 *        @ref Mutex::try_lock_for — see that method for the
	 *        `TimedLockable` non-satisfaction rationale.
	 *
	 * @param[in] rel Relative timeout (any `std::chrono::duration` unit).
	 * @return Empty `Result<void>` on acquisition; `unexpected`
	 *         @ref Error::Timeout if the deadline elapsed without
	 *         the lock being acquired; `unexpected` with another
	 *         @ref Error value on backend failure.
	 */
	template <class Rep, class Period>
	[[nodiscard]] Result<void>
	try_lock_for(const std::chrono::duration<Rep, Period> &rel) noexcept
	{
		return from_rc(ove_recursive_mutex_lock(handle_, to_timeout_ns(rel)));
	}

	/**
	 * @brief Deadline-based acquisition templated over the clock.
	 *
	 * Same clock-templating rationale as @ref Mutex::try_lock_until
	 * (deadline converted to a relative duration internally).
	 *
	 * @return As @ref try_lock_for — `Result<void>` with
	 *         `Error::Timeout` on timeout.
	 */
	template <class Clock, class Duration>
	[[nodiscard]] Result<void>
	try_lock_until(const std::chrono::time_point<Clock, Duration> &deadline) noexcept
	{
		const auto rel = deadline - Clock::now();
		return from_rc(ove_recursive_mutex_lock(handle_, to_timeout_ns(rel)));
	}

	/**
	 * @brief Releases one level of the recursive lock.
	 */
	void unlock()
	{
		ove_recursive_mutex_unlock(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the mutex was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS mutex handle.
	 * @return The opaque `ove_mutex_t` handle.
	 */
	ove_mutex_t handle() const
	{
		return handle_;
	}

      private:
	ove_mutex_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mutex_storage_t storage_ = {};
#endif
};

static_assert(detail::basic_lockable<RecursiveMutex>,
	      "ove::RecursiveMutex must satisfy the BasicLockable named requirement");
static_assert(detail::lockable<RecursiveMutex>,
	      "ove::RecursiveMutex must satisfy the Lockable named requirement");

/**
 * @class LockGuard
 * @brief Scoped RAII guard that locks a `Mutex` on construction and unlocks it on destruction.
 *
 * This is the oveRTOS equivalent of `std::lock_guard`.  The mutex is acquired
 * with an indefinite timeout; a failure to acquire is treated as a fatal error
 * via `OVE_STATIC_INIT_ASSERT`.
 *
 * @note Non-copyable and non-movable.
 */
class LockGuard
{
      public:
	/**
	 * @brief Constructs the guard, immediately locking the given mutex.
	 *
	 * Acquires @p mtx via @ref Mutex::lock (indefinite wait).  Failure
	 * is handled by @ref Mutex::lock itself (aborts via
	 * @c OVE_STATIC_INIT_ASSERT), so the destructor's
	 * @ref Mutex::unlock cannot run on a mutex we never acquired.
	 *
	 * @note Prefer `std::lock_guard<ove::Mutex>` for new code — now
	 * works since @ref Mutex satisfies `std::Lockable`.  @ref LockGuard
	 * is retained for code targeting older compiler standard libraries
	 * and for source-compatibility.
	 *
	 * @param[in] mtx The mutex to lock.  Must outlive this guard.
	 */
	explicit LockGuard(Mutex &mtx) : mtx_(mtx)
	{
		mtx_.lock();
	}

	/**
	 * @brief Destroys the guard, unlocking the associated mutex.
	 */
	~LockGuard() noexcept
	{
		mtx_.unlock();
	}

	LockGuard(const LockGuard &) = delete;
	LockGuard &operator=(const LockGuard &) = delete;
	LockGuard(LockGuard &&) = delete;
	LockGuard &operator=(LockGuard &&) = delete;

      private:
	Mutex &mtx_;
};

/**
 * @class StaticCell
 * @brief Thread-safe, lazily initialised storage cell for a single object of type `T`.
 *
 * Holds the object in a properly aligned raw byte buffer so that no dynamic
 * allocation is required.  The object is placement-constructed by calling
 * `init()` and its lifetime ends when the `StaticCell` is destroyed.  A
 * second call to `init()` aborts via assertion.
 *
 * This is useful for zero-heap embedded targets where global objects with
 * complex constructors must be initialised in a controlled order.
 *
 * @tparam T The type of the object to store.  Must be constructible with the
 *           arguments passed to `init()`.
 *
 * @note Non-copyable and non-movable.
 * @note **Thread-safety.** The cell publishes the `init() → get()`
 *       happens-before edge via an atomic flag, so once `init()` has
 *       returned on one thread, all subsequent `get()` calls on other
 *       threads observe a fully constructed object.  Beyond that, the
 *       cell does **not** synchronise access to the contained `T`:
 *       concurrent reads/mutations via the returned reference are the
 *       caller's responsibility — wrap the contents in your own
 *       @ref Mutex (or use a type that is internally thread-safe) if
 *       multiple threads will mutate it after publication.
 */
template <typename T> class StaticCell
{
      public:
	/**
	 * @brief Constructs the contained object in-place.
	 *
	 * Forwards all arguments to `T`'s constructor.  The initialisation flag is
	 * set atomically, ensuring the cell is initialised at most once.
	 *
	 * @tparam Args Constructor argument types.
	 * @param[in] args Arguments forwarded to `T`'s constructor.
	 * @return Reference to the newly constructed object.
	 */
	template <typename... Args> T &init(Args &&...args)
	{
		bool expected = false;
		if (!initialized_.compare_exchange_strong(expected, true))
			OVE_STATIC_INIT_ASSERT(false && "StaticCell already initialized");
		return *new (storage_) T(std::forward<Args>(args)...);
	}

	/**
	 * @brief Returns a reference to the contained object.
	 *
	 * Asserts if `init()` has not been called yet.
	 *
	 * The returned reference is unsynchronised — see the class-level
	 * thread-safety note: concurrent mutations through this reference
	 * are the caller's responsibility.
	 *
	 * @return Reference to the contained object.
	 */
	T &get()
	{
		OVE_STATIC_INIT_ASSERT(initialized_.load());
		return *reinterpret_cast<T *>(storage_);
	}

	/**
	 * @brief Returns a const reference to the contained object.
	 *
	 * Asserts if `init()` has not been called yet.
	 *
	 * The returned reference is unsynchronised — see the class-level
	 * thread-safety note.  Concurrent `const` reads are safe; if any
	 * thread mutates the contained object (via the non-`const`
	 * overload or via @c const_cast / @c mutable members), the caller
	 * must provide external synchronisation.
	 *
	 * @return Const reference to the contained object.
	 */
	const T &get() const
	{
		OVE_STATIC_INIT_ASSERT(initialized_.load());
		return *reinterpret_cast<const T *>(storage_);
	}

	/**
	 * @brief Returns `true` if `init()` has been called successfully.
	 * @return `true` when the cell contains a live object.
	 */
	bool is_initialized() const
	{
		return initialized_.load();
	}

	StaticCell() = default;
	~StaticCell() = default;
	StaticCell(const StaticCell &) = delete;
	StaticCell &operator=(const StaticCell &) = delete;
	StaticCell(StaticCell &&) = delete;
	StaticCell &operator=(StaticCell &&) = delete;

      private:
	alignas(T) uint8_t storage_[sizeof(T)]{};
	std::atomic<bool> initialized_{false};
};

/**
 * @class Semaphore
 * @brief RAII wrapper around an oveRTOS counting semaphore.
 *
 * A counting semaphore with a configurable initial count and maximum count.
 * Commonly used as a binary semaphore (`initial = 0, max = 1`) for signalling
 * between tasks or from an ISR.
 *
 * With `CONFIG_OVE_ZERO_HEAP` the semaphore storage is held inline and move
 * operations are disabled.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 * @note `take()` is marked `[[nodiscard]]`; the return value indicates
 *       whether the semaphore was actually decremented.
 */
class Semaphore
{
      public:
	/**
	 * @brief Constructs and initialises the semaphore.
	 * @param[in] initial Initial count value (default: 0).
	 * @param[in] max     Maximum count value (default: 1, binary semaphore).
	 *
	 * Asserts at startup if initialisation fails.
	 */
	explicit Semaphore(unsigned int initial = 0, unsigned int max = 1)
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_sem_init(&handle_, &storage_, initial, max);
#else
		int err = ove_sem_create(&handle_, initial, max);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the semaphore, releasing the underlying kernel resource.
	 */
	~Semaphore() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_sem_deinit(handle_);
#else
		ove_sem_destroy(handle_);
#endif
	}

	Semaphore(const Semaphore &) = delete;
	Semaphore &operator=(const Semaphore &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Semaphore(Semaphore &&) = delete;
	Semaphore &operator=(Semaphore &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Semaphore(Semaphore &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Semaphore &operator=(Semaphore &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_sem_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Decrements the semaphore count, blocking indefinitely.
	 *
	 * `std::counting_semaphore::acquire` analog.  Failure of an
	 * indefinite wait means the handle is unusable — programming
	 * error.  Aborts via @c OVE_STATIC_INIT_ASSERT on non-OK return
	 * (same shape as @ref Mutex::lock).
	 */
	void acquire()
	{
		const int err = ove_sem_take(handle_, OVE_WAIT_FOREVER);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Non-blocking acquisition attempt.  `std::counting_semaphore::try_acquire` analog.
	 * @return `true` on acquisition, `false` if the count was zero.
	 */
	[[nodiscard]] bool try_acquire()
	{
		return ove_sem_take(handle_, 0) == OVE_OK;
	}

	/**
	 * @brief Bounded-wait acquisition.
	 *
	 * Loosely analogous to @c std::counting_semaphore::try_acquire_for
	 * but with a `Result<void>` return instead of `bool` — timeout and
	 * backend errors are reported distinctly via @ref Error.  This
	 * means @c std::counting_semaphore's interface is **not** strictly
	 * satisfied; the standard has no concept for it, so the
	 * mismatch shows up only if you try to substitute the type into a
	 * generic template that expects the standard shape.
	 *
	 * @param[in] rel Relative timeout (any `std::chrono::duration` unit).
	 * @return Empty `Result<void>` on acquisition; `unexpected`
	 *         @ref Error::Timeout if the count was zero at the
	 *         deadline; `unexpected` with another @ref Error value on
	 *         backend failure.
	 */
	template <class Rep, class Period>
	[[nodiscard]] Result<void>
	try_acquire_for(const std::chrono::duration<Rep, Period> &rel) noexcept
	{
		return from_rc(ove_sem_take(handle_, to_timeout_ns(rel)));
	}

	/**
	 * @brief Deadline-based acquisition templated over the clock.
	 *
	 * Same clock-templating rationale as @ref Mutex::try_lock_until
	 * (deadline converted to a relative duration internally; clock's
	 * epoch need not match @c ove::steady_clock).
	 *
	 * @return As @ref try_acquire_for — `Result<void>` with
	 *         `Error::Timeout` on timeout.
	 */
	template <class Clock, class Duration>
	[[nodiscard]] Result<void>
	try_acquire_until(const std::chrono::time_point<Clock, Duration> &deadline) noexcept
	{
		const auto rel = deadline - Clock::now();
		return from_rc(ove_sem_take(handle_, to_timeout_ns(rel)));
	}

	/**
	 * @brief Increments the semaphore count, unblocking a waiting task if any.
	 *
	 * `std::counting_semaphore::release` analog.  Safe to call from
	 * both task and ISR context.  Increments by 1; batched form
	 * `release(unsigned n)` is not yet available pending substrate
	 * `ove_sem_give_n` (tracked in `c-substrate-findings.md`).
	 */
	void release()
	{
		ove_sem_give(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the semaphore was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS semaphore handle.
	 * @return The opaque `ove_sem_t` handle.
	 */
	ove_sem_t handle() const
	{
		return handle_;
	}

      private:
	ove_sem_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_sem_storage_t storage_ = {};
#endif
};

/**
 * @class Event
 * @brief RAII wrapper around an oveRTOS binary event flag.
 *
 * An `Event` is a lightweight synchronisation primitive for signalling a
 * single occurrence from one task (or ISR) to another.  The event is in an
 * unsignalled state after construction; `signal()` or `signal_from_isr()`
 * wakes any task blocked in `wait()`.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 */
class Event
{
      public:
	/**
	 * @brief Constructs and initialises the event in the unsignalled state.
	 *
	 * Asserts at startup if initialisation fails.
	 */
	Event()
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_event_init(&handle_, &storage_);
#else
		int err = ove_event_create(&handle_);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the event, releasing the underlying kernel resource.
	 */
	~Event() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_event_deinit(handle_);
#else
		ove_event_destroy(handle_);
#endif
	}

	Event(const Event &) = delete;
	Event &operator=(const Event &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	Event(Event &&) = delete;
	Event &operator=(Event &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Event(Event &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Event &operator=(Event &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_event_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Blocks the calling task until the event is signalled.
	 *
	 * Forever wait; failure means the handle is unusable.  Aborts via
	 * @c OVE_STATIC_INIT_ASSERT (same shape as @ref Mutex::lock and
	 * @ref Semaphore::acquire).
	 */
	void wait()
	{
		const int err = ove_event_wait(handle_, OVE_WAIT_FOREVER);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Non-blocking check.
	 * @return `true` if the event was signalled (and the wait consumed it),
	 *         `false` otherwise.
	 */
	[[nodiscard]] bool try_wait()
	{
		return ove_event_wait(handle_, 0) == OVE_OK;
	}

	/**
	 * @brief Bounded-wait.
	 *
	 * @param[in] rel Relative timeout (any `std::chrono::duration` unit).
	 * @return Empty `Result<void>` on signal (wait consumed it);
	 *         `unexpected` @ref Error::Timeout if no signal arrived by
	 *         the deadline; `unexpected` with another @ref Error
	 *         value on backend failure.
	 */
	template <class Rep, class Period>
	[[nodiscard]] Result<void>
	try_wait_for(const std::chrono::duration<Rep, Period> &rel) noexcept
	{
		return from_rc(ove_event_wait(handle_, to_timeout_ns(rel)));
	}

	/**
	 * @brief Deadline-based wait templated over the clock.
	 *
	 * Same clock-templating rationale as @ref Mutex::try_lock_until.
	 *
	 * @return As @ref try_wait_for — `Result<void>` with
	 *         `Error::Timeout` on timeout.
	 */
	template <class Clock, class Duration>
	[[nodiscard]] Result<void>
	try_wait_until(const std::chrono::time_point<Clock, Duration> &deadline) noexcept
	{
		const auto rel = deadline - Clock::now();
		return from_rc(ove_event_wait(handle_, to_timeout_ns(rel)));
	}

	/**
	 * @brief Signals the event from task context, waking any blocked waiter.
	 */
	void signal()
	{
		ove_event_signal(handle_);
	}

	/**
	 * @brief Signals the event from an ISR context, waking any blocked waiter.
	 *
	 * Must only be called from an interrupt service routine.
	 */
	void signal_from_isr()
	{
		ove_event_signal_from_isr(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the event was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS event handle.
	 * @return The opaque `ove_event_t` handle.
	 */
	ove_event_t handle() const
	{
		return handle_;
	}

      private:
	ove_event_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_event_storage_t storage_ = {};
#endif
};

/**
 * @class CondVar
 * @brief RAII wrapper around an oveRTOS condition variable.
 *
 * A condition variable allows tasks to efficiently wait for a predicate to
 * become true.  It must always be used together with a `Mutex` — the mutex
 * is atomically released while waiting and re-acquired before `wait()`
 * returns.
 *
 * @note Not copyable.  Move-only when heap allocation is enabled.
 * @note Always check the predicate in a loop after `wait()` to guard against
 *       spurious wake-ups.
 */
class CondVar
{
      public:
	/**
	 * @brief Constructs and initialises the condition variable.
	 *
	 * Asserts at startup if initialisation fails.
	 */
	CondVar()
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_condvar_init(&handle_, &storage_);
#else
		int err = ove_condvar_create(&handle_);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the condition variable, releasing the underlying kernel resource.
	 */
	~CondVar() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_condvar_deinit(handle_);
#else
		ove_condvar_destroy(handle_);
#endif
	}

	CondVar(const CondVar &) = delete;
	CondVar &operator=(const CondVar &) = delete;

#ifdef CONFIG_OVE_ZERO_HEAP
	CondVar(CondVar &&) = delete;
	CondVar &operator=(CondVar &&) = delete;
#else
	/**
	 * @brief Move constructor — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	CondVar(CondVar &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — transfers ownership of the kernel handle.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	CondVar &operator=(CondVar &&other) noexcept
	{
		if (this != &other) {
			if (handle_)
				ove_condvar_destroy(handle_);
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
#endif

	/**
	 * @brief Atomically releases the mutex and waits for a notification.
	 *
	 * The calling task must hold `mtx` before calling this method.  The mutex
	 * is released atomically while the task blocks, and re-acquired before the
	 * call returns.
	 *
	 * Forever wait; failure means the handle is unusable.  Aborts via
	 * @c OVE_STATIC_INIT_ASSERT.  Always re-check the predicate after
	 * @c wait returns to guard against spurious wake-ups.
	 *
	 * `std::condition_variable::wait` analog.
	 *
	 * @param[in] mtx The mutex associated with the predicate being
	 *                waited on.  Must be locked by the calling thread.
	 */
	void wait(Mutex &mtx)
	{
		const int err = ove_condvar_wait(handle_, mtx.handle(), OVE_WAIT_FOREVER);
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Bounded-wait.
	 *
	 * Loose analog of @c std::condition_variable::wait_for — but
	 * returns `Result<void>` rather than `std::cv_status`, matching
	 * the Result-shape convention used by the rest of the
	 * `ove::Mutex`/`Semaphore`/`Event` timeout-bearing methods.
	 *
	 * (`std::cv_status` is unavailable on the bare-metal backends —
	 * arm-gnu-toolchain's libstdc++ ships with `--enable-threads=single`,
	 * so neither `std::condition_variable` nor `std::cv_status` are
	 * declared.  Using `Result<void>` keeps the API uniform across
	 * POSIX, FreeRTOS, NuttX and Zephyr.)
	 *
	 * @param[in] mtx The mutex (locked by the calling thread).  The
	 *                kernel releases it while blocking and re-acquires
	 *                it before returning.
	 * @param[in] rel Relative timeout (any `std::chrono::duration` unit).
	 * @return Empty `Result<void>` on wake; `unexpected`
	 *         @ref Error::Timeout if the deadline elapsed without a
	 *         notification; `unexpected` with another @ref Error
	 *         value on backend failure.
	 *
	 * @note No `try_wait()` immediate form — a condvar wait is
	 *       inherently blocking; `std::condition_variable` matches
	 *       this (no `try_wait`).
	 * @note Always re-check the predicate in a loop after this
	 *       returns to guard against spurious wake-ups.  The
	 *       predicate-overload below does this internally.
	 */
	template <class Rep, class Period>
	[[nodiscard]] Result<void>
	try_wait_for(Mutex &mtx, const std::chrono::duration<Rep, Period> &rel) noexcept
	{
		return from_rc(ove_condvar_wait(handle_, mtx.handle(), to_timeout_ns(rel)));
	}

	/**
	 * @brief Deadline-based wait templated over the clock.
	 *
	 * Same clock-templating rationale as @ref Mutex::try_lock_until.
	 *
	 * @return As @ref try_wait_for — `Result<void>` with
	 *         `Error::Timeout` on timeout.
	 */
	template <class Clock, class Duration>
	[[nodiscard]] Result<void>
	try_wait_until(Mutex &mtx,
		       const std::chrono::time_point<Clock, Duration> &deadline) noexcept
	{
		const auto rel = deadline - Clock::now();
		return from_rc(ove_condvar_wait(handle_, mtx.handle(), to_timeout_ns(rel)));
	}

	/**
	 * @brief Predicate-loop wait.  `std::condition_variable::wait`
	 * predicate-overload analog.
	 *
	 * Equivalent to:
	 * @code
	 *   while (!pred()) wait(mtx);
	 * @endcode
	 * Handles spurious wake-ups internally; caller cannot accidentally
	 * write the `if (cv.wait(...) == OVE_OK && ready) { ... }`
	 * race-prone form.
	 *
	 * @param[in] mtx  The mutex (locked by the calling thread).
	 * @param[in] pred Callable returning bool.  Evaluated under @p mtx.
	 */
	template <typename Predicate> void wait(Mutex &mtx, Predicate pred)
	{
		while (!pred()) {
			wait(mtx);
		}
	}

	/**
	 * @brief Bounded-wait with predicate.
	 * `std::condition_variable::wait_for` predicate-overload analog.
	 *
	 * Loops on @p pred, returning when either @p pred() becomes true
	 * or @p rel elapses.
	 *
	 * @return `true` if @p pred() was true on return; `false` if
	 *         the timeout elapsed with @p pred() still false.
	 */
	template <typename Rep, typename Period, typename Predicate>
	[[nodiscard]] bool try_wait_for(Mutex &mtx, std::chrono::duration<Rep, Period> rel,
					Predicate pred)
	{
		const auto deadline = steady_clock::now() + rel;
		return try_wait_until(mtx, deadline, pred);
	}

	/**
	 * @brief Deadline-based wait with predicate.
	 * `std::condition_variable::wait_until` predicate-overload analog.
	 *
	 * Loops on @p pred, recomputing the remaining timeout on each
	 * iteration so spurious wake-ups don't shorten the effective
	 * deadline.
	 *
	 * @return `true` if @p pred() was true on return; `false` if
	 *         the deadline elapsed with @p pred() still false.
	 */
	template <typename Clock, typename Duration, typename Predicate>
	[[nodiscard]] bool try_wait_until(Mutex &mtx,
					  const std::chrono::time_point<Clock, Duration> &deadline,
					  Predicate pred)
	{
		while (!pred()) {
			const auto now = Clock::now();
			if (now >= deadline)
				return pred();
			const int rc = ove_condvar_wait(handle_, mtx.handle(),
							to_timeout_ns(deadline - now));
			(void)rc; /* spurious wake-up or timeout — re-check pred */
		}
		return true;
	}

	/**
	 * @brief Wakes one task waiting on this condition variable.
	 * `std::condition_variable::notify_one` analog.
	 */
	void notify_one()
	{
		ove_condvar_signal(handle_);
	}

	/**
	 * @brief Wakes all tasks waiting on this condition variable.
	 * `std::condition_variable::notify_all` analog.
	 */
	void notify_all()
	{
		ove_condvar_broadcast(handle_);
	}

	/**
	 * @brief Returns `true` if the underlying kernel handle is non-null.
	 * @return `true` when the condition variable was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS condition variable handle.
	 * @return The opaque `ove_condvar_t` handle.
	 */
	ove_condvar_t handle() const
	{
		return handle_;
	}

      private:
	ove_condvar_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_condvar_storage_t storage_ = {};
#endif
};

} // namespace ove

#endif /* CONFIG_OVE_SYNC */
