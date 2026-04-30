// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Synchronisation primitives.
//!
//! All wrappers follow the same idiomatic pattern:
//!
//! ```zig
//! var mtx: ove.Mutex = undefined;
//! try mtx.init();
//! defer mtx.deinit();
//! try mtx.lock(ove.wait_forever);
//! mtx.unlock();
//! ```
//!
//! ## Pinning contract
//!
//! Under `CONFIG_OVE_ZERO_HEAP=y` each wrapper embeds the kernel-object
//! storage as a struct field, and the kernel handle stored in `self.handle`
//! references `&self.storage`.  After `init()` the wrapper **must not be
//! moved, copied, passed by value, or relocated** — doing so invalidates
//! the kernel pointer and silently corrupts RTOS state.  Debug builds
//! (`std.debug.runtime_safety == true`) record `&self` at `init()` and
//! panic if any method later sees a different address.  Release builds
//! compile the check out at zero cost.
//!
//! `init()` failure leaves `self` in its original `undefined` state — do
//! not register `defer self.deinit()` until after `try self.init()`
//! succeeds.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

/// Non-recursive mutual exclusion lock.
pub const Mutex = struct {
    storage: pin.Storage(c.ove_mutex_storage_t),
    handle: c.ove_mutex_t,
    tracker: pin.Tracker,

    /// Attach a kernel mutex to this wrapper.  Must be called exactly once
    /// before any locking operation, and `self` must outlive every
    /// subsequent call (the kernel handle points into `self.storage`).
    pub fn init(self: *Mutex) Error!void {
        self.storage = pin.zeroStorage(c.ove_mutex_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_mutex_create(&self.handle));
        } else {
            try err.fromCode(c.ove_mutex_init(&self.handle, &self.storage));
        }
        self.tracker.record(self);
    }

    /// Release kernel resources.  Idempotent.  Must be called from the
    /// same address as `init()`.
    pub fn deinit(self: *Mutex) void {
        self.tracker.assertSame(self, "ove.Mutex");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_mutex_destroy(self.handle)
        else
            c.ove_mutex_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn lock(self: *Mutex, timeout_ms: u32) Error!void {
        self.tracker.assertSame(self, "ove.Mutex");
        try err.fromCode(c.ove_mutex_lock(self.handle, timeout_ms));
    }

    pub fn unlock(self: *Mutex) void {
        self.tracker.assertSame(self, "ove.Mutex");
        c.ove_mutex_unlock(self.handle);
    }

    /// RAII guard returned by `acquire()`.  Holds the mutex until
    /// `release()` is called (typically via `defer`).
    pub const Guard = struct {
        mutex: *Mutex,
        pub fn release(self: Guard) void {
            self.mutex.unlock();
        }
    };

    /// Lock and return a `Guard`:
    /// ```zig
    /// const g = try mtx.acquire(ove.wait_forever);
    /// defer g.release();
    /// ```
    pub fn acquire(self: *Mutex, timeout_ms: u32) Error!Guard {
        try self.lock(timeout_ms);
        return .{ .mutex = self };
    }
};

// ---------------------------------------------------------------------------
// RecursiveMutex
// ---------------------------------------------------------------------------

/// Recursive mutual exclusion lock.  The same task may lock multiple times;
/// each `lock()` must be paired with `unlock()`.
pub const RecursiveMutex = struct {
    storage: pin.Storage(c.ove_mutex_storage_t),
    handle: c.ove_mutex_t,
    tracker: pin.Tracker,

    pub fn init(self: *RecursiveMutex) Error!void {
        self.storage = pin.zeroStorage(c.ove_mutex_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_recursive_mutex_create(&self.handle));
        } else {
            try err.fromCode(c.ove_recursive_mutex_init(&self.handle, &self.storage));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *RecursiveMutex) void {
        self.tracker.assertSame(self, "ove.RecursiveMutex");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_mutex_destroy(self.handle)
        else
            c.ove_mutex_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn lock(self: *RecursiveMutex, timeout_ms: u32) Error!void {
        self.tracker.assertSame(self, "ove.RecursiveMutex");
        try err.fromCode(c.ove_recursive_mutex_lock(self.handle, timeout_ms));
    }

    pub fn unlock(self: *RecursiveMutex) void {
        self.tracker.assertSame(self, "ove.RecursiveMutex");
        c.ove_recursive_mutex_unlock(self.handle);
    }

    pub const Guard = struct {
        mutex: *RecursiveMutex,
        pub fn release(self: Guard) void {
            self.mutex.unlock();
        }
    };

    pub fn acquire(self: *RecursiveMutex, timeout_ms: u32) Error!Guard {
        try self.lock(timeout_ms);
        return .{ .mutex = self };
    }
};

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

/// Counting semaphore for signalling between tasks or from ISRs.
pub const Semaphore = struct {
    storage: pin.Storage(c.ove_sem_storage_t),
    handle: c.ove_sem_t,
    tracker: pin.Tracker,

    pub fn init(self: *Semaphore, initial: u32, max: u32) Error!void {
        self.storage = pin.zeroStorage(c.ove_sem_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_sem_create(&self.handle, initial, max));
        } else {
            try err.fromCode(c.ove_sem_init(&self.handle, &self.storage, initial, max));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *Semaphore) void {
        self.tracker.assertSame(self, "ove.Semaphore");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_sem_destroy(self.handle)
        else
            c.ove_sem_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    /// Decrement the semaphore count, blocking up to `timeout_ms`.
    pub fn take(self: *Semaphore, timeout_ms: u32) Error!void {
        self.tracker.assertSame(self, "ove.Semaphore");
        try err.fromCode(c.ove_sem_take(self.handle, timeout_ms));
    }

    /// Increment the semaphore count.  Safe to call from ISR context.
    pub fn give(self: *Semaphore) void {
        self.tracker.assertSame(self, "ove.Semaphore");
        c.ove_sem_give(self.handle);
    }
};

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

/// Binary event flag — auto-resets after a successful `wait()`.
pub const Event = struct {
    storage: pin.Storage(c.ove_event_storage_t),
    handle: c.ove_event_t,
    tracker: pin.Tracker,

    pub fn init(self: *Event) Error!void {
        self.storage = pin.zeroStorage(c.ove_event_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_event_create(&self.handle));
        } else {
            try err.fromCode(c.ove_event_init(&self.handle, &self.storage));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *Event) void {
        self.tracker.assertSame(self, "ove.Event");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_event_destroy(self.handle)
        else
            c.ove_event_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn wait(self: *Event, timeout_ms: u32) Error!void {
        self.tracker.assertSame(self, "ove.Event");
        try err.fromCode(c.ove_event_wait(self.handle, timeout_ms));
    }

    pub fn signal(self: *Event) void {
        self.tracker.assertSame(self, "ove.Event");
        c.ove_event_signal(self.handle);
    }

    pub fn signalFromIsr(self: *Event) void {
        // ISR path — skip pin check (panics from ISR context are unsafe);
        // the assumption is that init() ran from task context and the
        // wrapper is at a stable address by the time an ISR fires.
        c.ove_event_signal_from_isr(self.handle);
    }
};

// ---------------------------------------------------------------------------
// CondVar
// ---------------------------------------------------------------------------

/// Condition variable paired with a `Mutex` for producer/consumer patterns.
pub const CondVar = struct {
    storage: pin.Storage(c.ove_condvar_storage_t),
    handle: c.ove_condvar_t,
    tracker: pin.Tracker,

    pub fn init(self: *CondVar) Error!void {
        self.storage = pin.zeroStorage(c.ove_condvar_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_condvar_create(&self.handle));
        } else {
            try err.fromCode(c.ove_condvar_init(&self.handle, &self.storage));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *CondVar) void {
        self.tracker.assertSame(self, "ove.CondVar");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_condvar_destroy(self.handle)
        else
            c.ove_condvar_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    /// Atomically release `mutex` and wait for a signal/broadcast.
    /// Re-acquires `mutex` before returning.
    pub fn wait(self: *CondVar, mutex: *Mutex, timeout_ms: u32) Error!void {
        self.tracker.assertSame(self, "ove.CondVar");
        try err.fromCode(c.ove_condvar_wait(self.handle, mutex.handle, timeout_ms));
    }

    pub fn signal(self: *CondVar) void {
        self.tracker.assertSame(self, "ove.CondVar");
        c.ove_condvar_signal(self.handle);
    }

    pub fn broadcast(self: *CondVar) void {
        self.tracker.assertSame(self, "ove.CondVar");
        c.ove_condvar_broadcast(self.handle);
    }
};
