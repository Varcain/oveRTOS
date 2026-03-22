// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

/// Non-recursive mutual exclusion lock.
///
/// Supports both heap-allocated (`ove_mutex_create`) and zero-heap
/// (`ove_mutex_init`) backends, selected at compile time.
/// Use `acquire()` for RAII-style locking.
pub const Mutex = struct {
    handle: c.ove_mutex_t,

    /// Create and return a new mutex.
    ///
    /// In zero-heap mode, the storage is allocated inside a comptime-unique
    /// static variable — only one instance per call site is supported.
    /// Returns `Error` if the RTOS fails to create the mutex.
    pub fn create() Error!Mutex {
        var h: c.ove_mutex_t = null;
        if (comptime @hasDecl(c, "ove_mutex_create")) {
            try err.fromCode(c.ove_mutex_create(&h));
        } else {
            const S = struct {
                var storage: c.ove_mutex_storage_t = std.mem.zeroes(c.ove_mutex_storage_t);
            };
            try err.fromCode(c.ove_mutex_init(&h, &S.storage));
        }
        return .{ .handle = h };
    }

    /// Destroy the mutex and release underlying RTOS resources.
    ///
    /// Sets `handle` to null to prevent double-free. Safe to call on an
    /// already-destroyed mutex.
    pub fn destroy(self: *Mutex) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_mutex_destroy"))
            c.ove_mutex_destroy(self.handle)
        else
            c.ove_mutex_deinit(self.handle);
        self.handle = null;
    }

    /// Lock the mutex, blocking up to `timeout_ms` milliseconds.
    ///
    /// Use `wait_forever` for an indefinite block. Returns `Error.Timeout`
    /// if the lock is not acquired within the timeout.
    pub fn lock(self: Mutex, timeout_ms: u32) Error!void {
        try err.fromCode(c.ove_mutex_lock(self.handle, timeout_ms));
    }

    /// Release the mutex. Must be called from the task that locked it.
    pub fn unlock(self: Mutex) void {
        c.ove_mutex_unlock(self.handle);
    }

    /// RAII guard returned by `acquire()`.
    ///
    /// Holds the mutex until `release()` is called (typically via `defer`).
    pub const MutexGuard = struct {
        mutex: Mutex,

        /// Release the mutex held by this guard.
        pub fn release(self: MutexGuard) void {
            self.mutex.unlock();
        }
    };

    /// Alias for `MutexGuard` for convenience.
    pub const Guard = MutexGuard;

    /// Lock the mutex and return a Guard. Use with defer:
    ///     const guard = try mutex.acquire(timeout);
    ///     defer guard.release();
    pub fn acquire(self: Mutex, timeout_ms: u32) Error!MutexGuard {
        try self.lock(timeout_ms);
        return .{ .mutex = self };
    }
};

// ---------------------------------------------------------------------------
// RecursiveMutex
// ---------------------------------------------------------------------------

/// Recursive mutual exclusion lock.
///
/// The same task may lock this mutex multiple times without deadlocking.
/// Each `lock()` must be paired with a corresponding `unlock()`.
/// Supports both heap and zero-heap backends.
pub const RecursiveMutex = struct {
    handle: c.ove_mutex_t,

    /// Create and return a new recursive mutex.
    ///
    /// In zero-heap mode, the storage is a comptime-unique static variable.
    /// Returns `Error` if the RTOS fails to create the mutex.
    pub fn create() Error!RecursiveMutex {
        var h: c.ove_mutex_t = null;
        if (comptime @hasDecl(c, "ove_recursive_mutex_create")) {
            try err.fromCode(c.ove_recursive_mutex_create(&h));
        } else {
            const S = struct {
                var storage: c.ove_mutex_storage_t = std.mem.zeroes(c.ove_mutex_storage_t);
            };
            try err.fromCode(c.ove_recursive_mutex_init(&h, &S.storage));
        }
        return .{ .handle = h };
    }

    /// Destroy the recursive mutex and release underlying RTOS resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed mutex.
    pub fn destroy(self: *RecursiveMutex) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_mutex_destroy"))
            c.ove_mutex_destroy(self.handle)
        else
            c.ove_mutex_deinit(self.handle);
        self.handle = null;
    }

    /// Lock the recursive mutex, blocking up to `timeout_ms` milliseconds.
    ///
    /// May be called multiple times from the same task. Returns `Error.Timeout`
    /// if the lock is not acquired within the timeout.
    pub fn lock(self: RecursiveMutex, timeout_ms: u32) Error!void {
        try err.fromCode(c.ove_recursive_mutex_lock(self.handle, timeout_ms));
    }

    /// Release one level of the recursive lock.
    ///
    /// Must be called once for each successful `lock()`.
    pub fn unlock(self: RecursiveMutex) void {
        c.ove_recursive_mutex_unlock(self.handle);
    }

    /// RAII guard returned by `acquire()`.
    ///
    /// Holds one lock level until `release()` is called (typically via `defer`).
    pub const MutexGuard = struct {
        mutex: RecursiveMutex,

        /// Release one level of the recursive mutex held by this guard.
        pub fn release(self: MutexGuard) void {
            self.mutex.unlock();
        }
    };

    /// Alias for `MutexGuard` for convenience.
    pub const Guard = MutexGuard;

    /// Lock the mutex and return a Guard. Use with defer:
    ///     const guard = try rmutex.acquire(timeout);
    ///     defer guard.release();
    pub fn acquire(self: RecursiveMutex, timeout_ms: u32) Error!MutexGuard {
        try self.lock(timeout_ms);
        return .{ .mutex = self };
    }
};

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

/// Counting semaphore for signalling between tasks or from ISRs.
///
/// Supports both heap and zero-heap backends.
pub const Semaphore = struct {
    handle: c.ove_sem_t,

    /// Create a counting semaphore with an initial count and a maximum count.
    ///
    /// `initial` sets the starting count. `max` caps the count ceiling.
    /// In zero-heap mode, storage is a comptime-unique static variable.
    /// Returns `Error` if the RTOS fails to create the semaphore.
    pub fn create(initial: u32, max: u32) Error!Semaphore {
        var h: c.ove_sem_t = null;
        if (comptime @hasDecl(c, "ove_sem_create")) {
            try err.fromCode(c.ove_sem_create(&h, initial, max));
        } else {
            const S = struct {
                var storage: c.ove_sem_storage_t = std.mem.zeroes(c.ove_sem_storage_t);
            };
            try err.fromCode(c.ove_sem_init(&h, &S.storage, initial, max));
        }
        return .{ .handle = h };
    }

    /// Destroy the semaphore and release underlying RTOS resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed semaphore.
    pub fn destroy(self: *Semaphore) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_sem_destroy"))
            c.ove_sem_destroy(self.handle)
        else
            c.ove_sem_deinit(self.handle);
        self.handle = null;
    }

    /// Decrement the semaphore count, blocking up to `timeout_ms` milliseconds.
    ///
    /// Use `wait_forever` for an indefinite block. Returns `Error.Timeout`
    /// if the count does not become positive within the timeout.
    pub fn take(self: Semaphore, timeout_ms: u32) Error!void {
        try err.fromCode(c.ove_sem_take(self.handle, timeout_ms));
    }

    /// Increment the semaphore count, potentially unblocking a waiting task.
    ///
    /// Safe to call from ISR context.
    pub fn give(self: Semaphore) void {
        c.ove_sem_give(self.handle);
    }
};

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

/// Binary event flag for one-shot signalling between tasks or from ISRs.
///
/// A waiting task blocks until the event is signalled. The event is
/// automatically reset after a successful `wait()`.
/// Supports both heap and zero-heap backends.
pub const Event = struct {
    handle: c.ove_event_t,

    /// Create and return a new binary event (initially unsignalled).
    ///
    /// In zero-heap mode, storage is a comptime-unique static variable.
    /// Returns `Error` if the RTOS fails to create the event.
    pub fn create() Error!Event {
        var h: c.ove_event_t = null;
        if (comptime @hasDecl(c, "ove_event_create")) {
            try err.fromCode(c.ove_event_create(&h));
        } else {
            const S = struct {
                var storage: c.ove_event_storage_t = std.mem.zeroes(c.ove_event_storage_t);
            };
            try err.fromCode(c.ove_event_init(&h, &S.storage));
        }
        return .{ .handle = h };
    }

    /// Destroy the event and release underlying RTOS resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed event.
    pub fn destroy(self: *Event) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_event_destroy"))
            c.ove_event_destroy(self.handle)
        else
            c.ove_event_deinit(self.handle);
        self.handle = null;
    }

    /// Block until the event is signalled or `timeout_ms` elapses.
    ///
    /// Returns `Error.Timeout` if the event is not signalled in time.
    pub fn wait(self: Event, timeout_ms: u32) Error!void {
        try err.fromCode(c.ove_event_wait(self.handle, timeout_ms));
    }

    /// Signal the event, unblocking one waiting task.
    ///
    /// Safe to call from task context. For ISR context use `signalFromIsr()`.
    pub fn signal(self: Event) void {
        c.ove_event_signal(self.handle);
    }

    /// Signal the event from an interrupt service routine.
    ///
    /// Must be called only from ISR context. Does not perform a context switch
    /// immediately; the scheduler will yield when the ISR returns.
    pub fn signalFromIsr(self: Event) void {
        c.ove_event_signal_from_isr(self.handle);
    }
};

// ---------------------------------------------------------------------------
// CondVar
// ---------------------------------------------------------------------------

/// Condition variable for producer/consumer synchronization.
///
/// Must always be used together with a `Mutex`. A waiting task atomically
/// releases the mutex and sleeps until `signal()` or `broadcast()` is called,
/// then re-acquires the mutex before returning.
/// Supports both heap and zero-heap backends.
pub const CondVar = struct {
    handle: c.ove_condvar_t,

    /// Create and return a new condition variable.
    ///
    /// In zero-heap mode, storage is a comptime-unique static variable.
    /// Returns `Error` if the RTOS fails to create the condition variable.
    pub fn create() Error!CondVar {
        var h: c.ove_condvar_t = null;
        if (comptime @hasDecl(c, "ove_condvar_create")) {
            try err.fromCode(c.ove_condvar_create(&h));
        } else {
            const S = struct {
                var storage: c.ove_condvar_storage_t = std.mem.zeroes(c.ove_condvar_storage_t);
            };
            try err.fromCode(c.ove_condvar_init(&h, &S.storage));
        }
        return .{ .handle = h };
    }

    /// Destroy the condition variable and release underlying RTOS resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed condvar.
    pub fn destroy(self: *CondVar) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_condvar_destroy"))
            c.ove_condvar_destroy(self.handle)
        else
            c.ove_condvar_deinit(self.handle);
        self.handle = null;
    }

    /// Atomically release `mutex` and wait for a `signal()` or `broadcast()`.
    ///
    /// Blocks up to `timeout_ms` milliseconds. Re-acquires `mutex` before
    /// returning regardless of the outcome. Returns `Error.Timeout` if the
    /// condition is not signalled in time.
    pub fn wait(self: CondVar, mutex: Mutex, timeout_ms: u32) Error!void {
        try err.fromCode(c.ove_condvar_wait(self.handle, mutex.handle, timeout_ms));
    }

    /// Wake one task waiting on this condition variable.
    pub fn signal(self: CondVar) void {
        c.ove_condvar_signal(self.handle);
    }

    /// Wake all tasks waiting on this condition variable.
    pub fn broadcast(self: CondVar) void {
        c.ove_condvar_broadcast(self.handle);
    }
};
