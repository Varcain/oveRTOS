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
	 * @brief Acquires the mutex, blocking until it is available or the timeout expires.
	 * @param[in] timeout `std::chrono::duration`; defaults to `ove::wait_forever`.
	 *                   Pass any duration unit: `100ms`, `5s`, `500us`, etc.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int lock(std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_mutex_lock(handle_, to_timeout_ns(timeout));
	}

	/**
	 * @brief Deadline-based variant of @ref lock.
	 * @param[in] deadline @ref ove::steady_clock::time_point at which the
	 *                     wait must complete.  Pass
	 *                     `steady_clock::time_point::max()` to block
	 *                     indefinitely.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int lock_until(steady_clock::time_point deadline)
	{
		return ove_mutex_lock_until(handle_, to_deadline_ns(deadline));
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
	 * @brief Acquires the recursive mutex.
	 * @param[in] timeout_ns Maximum wait time in nanoseconds; use
	 *            `OVE_WAIT_FOREVER` to block indefinitely.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int lock(std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_recursive_mutex_lock(handle_, to_timeout_ns(timeout));
	}

	/**
	 * @brief Deadline-based variant of @ref lock.
	 * @param[in] deadline @ref ove::steady_clock::time_point at which the
	 *                     wait must complete.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int lock_until(steady_clock::time_point deadline)
	{
		return ove_recursive_mutex_lock_until(handle_, to_deadline_ns(deadline));
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
	 * @param[in] mtx The mutex to lock.  Must outlive this guard.
	 */
	explicit LockGuard(Mutex &mtx) : mtx_(mtx)
	{
		(void)mtx_.lock(); /* wait forever — failure is fatal */
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
 * @note `init()` and `get()` are safe to call from a single initialisation
 *       thread.  Concurrent `get()` calls after `init()` completes are safe
 *       due to the atomic flag, but the cell does not protect the contained
 *       object itself.
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
	 * @brief Decrements the semaphore count, blocking if the count is zero.
	 * @param[in] timeout_ns Maximum time to wait in nanoseconds; use
	 *            `OVE_WAIT_FOREVER` to block indefinitely.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int take(std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_sem_take(handle_, to_timeout_ns(timeout));
	}

	/**
	 * @brief Deadline-based variant of @ref take.
	 * @param[in] deadline @ref ove::steady_clock::time_point at which the
	 *                     wait must complete.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int take_until(steady_clock::time_point deadline)
	{
		return ove_sem_take_until(handle_, to_deadline_ns(deadline));
	}

	/**
	 * @brief Increments the semaphore count, unblocking a waiting task if any.
	 *
	 * Safe to call from both task and ISR context.
	 */
	void give()
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
	 * @brief Blocks the calling task until the event is signalled or the timeout expires.
	 * @param[in] timeout_ns Maximum time to wait in nanoseconds; use
	 *            `OVE_WAIT_FOREVER` to block indefinitely.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int wait(std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_event_wait(handle_, to_timeout_ns(timeout));
	}

	/**
	 * @brief Deadline-based variant of @ref wait.
	 * @param[in] deadline @ref ove::steady_clock::time_point at which the
	 *                     wait must complete.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int wait_until(steady_clock::time_point deadline)
	{
		return ove_event_wait_until(handle_, to_deadline_ns(deadline));
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
	 * @param[in] mtx        The mutex associated with the predicate being waited on.
	 * @param[in] timeout_ns Maximum wait time in nanoseconds; use
	 *            `OVE_WAIT_FOREVER` to block indefinitely.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int wait(Mutex &mtx, std::chrono::nanoseconds timeout = wait_forever)
	{
		return ove_condvar_wait(handle_, mtx.handle(), to_timeout_ns(timeout));
	}

	/**
	 * @brief Deadline-based variant of @ref wait.
	 * @param[in] mtx      Mutex that guards the condition.  Must be locked
	 *                     by the calling thread.
	 * @param[in] deadline @ref ove::steady_clock::time_point at which the
	 *                     wait must complete.
	 * @return `OVE_OK` on success, or a negative error code on timeout/failure.
	 */
	[[nodiscard]] int wait_until(Mutex &mtx, steady_clock::time_point deadline)
	{
		return ove_condvar_wait_until(handle_, mtx.handle(), to_deadline_ns(deadline));
	}

	/**
	 * @brief Wakes one task waiting on this condition variable.
	 */
	void signal()
	{
		ove_condvar_signal(handle_);
	}

	/**
	 * @brief Wakes all tasks waiting on this condition variable.
	 */
	void broadcast()
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
