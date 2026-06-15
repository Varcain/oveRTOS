// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Synchronisation primitives.
//!
//! All primitives use the substrate's `_init` path with caller-provided
//! storage allocated from a `std.mem.Allocator`.  Works uniformly in heap
//! and zero-heap builds — heap-mode users typically pass
//! `ove.allocators.c_allocator` (a libc-backed wrapper that routes
//! through the substrate's heap policy); zero-heap-mode users pass a
//! `FixedBufferAllocator` backed by a static byte buffer in `.bss`
//! (constructed via `ove.fixedBufferAlloc`).
//!
//! ```zig
//! var mtx = try ove.Mutex.create(allocator);
//! defer mtx.deinit();
//! mtx.lock();
//! defer mtx.unlock();
//! ```
//!
//! The wrappers are movable for operations — every operation method
//! (`lock`, `wait`, …) dereferences the allocator-managed storage via the
//! stable pointer the wrapper carries, so passing the wrapper by value
//! across function boundaries is safe.  No `pin.Tracker` needed at the
//! wrapper level (the storage lives at the allocator's stable address;
//! only the wrapper's two-word handle moves).
//!
//! Ownership is single-owner: `deinit` takes `*Self` and clears the
//! wrapper's handle/storage, so call it on the owning variable rather than
//! a by-value copy.  `deinit` is idempotent — a redundant `defer
//! x.deinit()` after an explicit `deinit()` is a safe no-op, not a double
//! free.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

const time_mod = @import("time.zig");
const Duration = time_mod.Duration;
const Instant = time_mod.Instant;
const WAIT_FOREVER = c.OVE_WAIT_FOREVER;

// ---------------------------------------------------------------------------
// Per-operation narrow error sets (A3).
// ---------------------------------------------------------------------------

/// Error set for `Mutex.lock*` / `RecursiveMutex.lock*` / `acquire*`.
pub const LockError = error{Timeout};

/// Error set for `Semaphore.timedWait*` / `Event.timedWait*` /
/// `CondVar.timedWait*`.
pub const WaitError = error{Timeout};

inline fn panicUnexpected(comptime ctx: []const u8, rc: c_int) noreturn {
    std.debug.panic("ove." ++ ctx ++ ": unexpected substrate rc {d}", .{rc});
}

inline fn mapTimeoutOnly(comptime ctx: []const u8, rc: c_int) error{Timeout} {
    return switch (rc) {
        c.OVE_ERR_TIMEOUT => error.Timeout,
        else => panicUnexpected(ctx, rc),
    };
}

/// `tryX()` helper: timeout=0 → OK → true, Timeout → false, else panic.
inline fn tryRc(comptime ctx: []const u8, rc: c_int) bool {
    return switch (rc) {
        0 => true,
        c.OVE_ERR_TIMEOUT => false,
        else => panicUnexpected(ctx, rc),
    };
}

/// Forever-blocking helper: panic on programming-bug substrate codes.
inline fn panicOnRc(comptime ctx: []const u8, rc: c_int) void {
    if (rc < 0) panicUnexpected(ctx, rc);
}

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

/// Non-recursive mutual exclusion lock.
pub const Mutex = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_mutex_t,
    storage: ?*c.ove_mutex_storage_t,

    /// Allocate a substrate-storage block from `allocator` and
    /// `ove_mutex_init` against it.  Wrapper is movable by value for
    /// operations; `deinit` is single-owner and idempotent.
    pub fn create(allocator: std.mem.Allocator) Error!Mutex {
        const storage = try allocator.create(c.ove_mutex_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_mutex_storage_t);
        var h: c.ove_mutex_t = null;
        try err.fromCode(c.ove_mutex_init(&h, storage));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: *Mutex) void {
        if (self.handle) |handle| {
            c.ove_mutex_deinit(handle);
            self.handle = null;
        }
        if (self.storage) |storage| {
            self.allocator.destroy(storage);
            self.storage = null;
        }
    }

    /// Acquire the lock, blocking indefinitely.  Infallible.
    /// `std.Thread.Mutex.lock` mirror.
    pub inline fn lock(self: Mutex) void {
        const rc = c.ove_mutex_lock(self.handle, WAIT_FOREVER);
        panicOnRc("Mutex.lock", rc);
    }

    /// Non-blocking lock attempt.  `std.Thread.Mutex.tryLock` mirror.
    pub inline fn tryLock(self: Mutex) bool {
        return tryRc("Mutex.tryLock", c.ove_mutex_lock(self.handle, 0));
    }

    /// Lock with a bounded duration.
    pub inline fn lockFor(self: Mutex, d: Duration) LockError!void {
        const rc = c.ove_mutex_lock(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Mutex.lockFor", rc);
    }

    /// Lock with an absolute deadline.
    pub inline fn lockUntil(self: Mutex, deadline: Instant) LockError!void {
        const t = time_mod.deadlineToTimeoutNs(deadline);
        const rc = c.ove_mutex_lock(self.handle, t);
        if (rc < 0) return mapTimeoutOnly("Mutex.lockUntil", rc);
    }

    pub inline fn unlock(self: Mutex) void {
        c.ove_mutex_unlock(self.handle);
    }

    /// RAII guard.  Holds the mutex until `release()`.
    pub const Guard = struct {
        mutex: Mutex,
        pub fn release(self: Guard) void {
            self.mutex.unlock();
        }
    };

    pub fn acquire(self: Mutex) Guard {
        self.lock();
        return .{ .mutex = self };
    }

    pub fn acquireFor(self: Mutex, d: Duration) LockError!Guard {
        try self.lockFor(d);
        return .{ .mutex = self };
    }

    pub fn acquireUntil(self: Mutex, deadline: Instant) LockError!Guard {
        try self.lockUntil(deadline);
        return .{ .mutex = self };
    }
};

// ---------------------------------------------------------------------------
// RecursiveMutex
// ---------------------------------------------------------------------------

/// Recursive mutual exclusion lock.  Same thread may lock multiple times;
/// each lock must be paired with an unlock.
pub const RecursiveMutex = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_mutex_t,
    storage: ?*c.ove_mutex_storage_t,

    /// Allocate a substrate-storage block and `ove_recursive_mutex_init`
    /// against it.
    pub fn create(allocator: std.mem.Allocator) Error!RecursiveMutex {
        const storage = try allocator.create(c.ove_mutex_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_mutex_storage_t);
        var h: c.ove_mutex_t = null;
        try err.fromCode(c.ove_recursive_mutex_init(&h, storage));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: *RecursiveMutex) void {
        if (self.handle) |handle| {
            c.ove_mutex_deinit(handle);
            self.handle = null;
        }
        if (self.storage) |storage| {
            self.allocator.destroy(storage);
            self.storage = null;
        }
    }

    pub inline fn lock(self: RecursiveMutex) void {
        const rc = c.ove_recursive_mutex_lock(self.handle, WAIT_FOREVER);
        panicOnRc("RecursiveMutex.lock", rc);
    }

    pub inline fn tryLock(self: RecursiveMutex) bool {
        return tryRc("RecursiveMutex.tryLock", c.ove_recursive_mutex_lock(self.handle, 0));
    }

    pub inline fn lockFor(self: RecursiveMutex, d: Duration) LockError!void {
        const rc = c.ove_recursive_mutex_lock(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("RecursiveMutex.lockFor", rc);
    }

    pub inline fn lockUntil(self: RecursiveMutex, deadline: Instant) LockError!void {
        const t = time_mod.deadlineToTimeoutNs(deadline);
        const rc = c.ove_recursive_mutex_lock(self.handle, t);
        if (rc < 0) return mapTimeoutOnly("RecursiveMutex.lockUntil", rc);
    }

    pub inline fn unlock(self: RecursiveMutex) void {
        c.ove_recursive_mutex_unlock(self.handle);
    }

    pub const Guard = struct {
        mutex: RecursiveMutex,
        pub fn release(self: Guard) void {
            self.mutex.unlock();
        }
    };

    pub fn acquire(self: RecursiveMutex) Guard {
        self.lock();
        return .{ .mutex = self };
    }

    pub fn acquireFor(self: RecursiveMutex, d: Duration) LockError!Guard {
        try self.lockFor(d);
        return .{ .mutex = self };
    }

    pub fn acquireUntil(self: RecursiveMutex, deadline: Instant) LockError!Guard {
        try self.lockUntil(deadline);
        return .{ .mutex = self };
    }
};

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

/// Counting semaphore for signalling between tasks or from ISRs.
pub const Semaphore = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_sem_t,
    storage: ?*c.ove_sem_storage_t,

    /// Create a counting semaphore with `initial` permits available
    /// out of a maximum of `max`.
    pub fn create(allocator: std.mem.Allocator, initial: u32, max: u32) Error!Semaphore {
        const storage = try allocator.create(c.ove_sem_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_sem_storage_t);
        var h: c.ove_sem_t = null;
        try err.fromCode(c.ove_sem_init(&h, storage, initial, max));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: *Semaphore) void {
        if (self.handle) |handle| {
            c.ove_sem_deinit(handle);
            self.handle = null;
        }
        if (self.storage) |storage| {
            self.allocator.destroy(storage);
            self.storage = null;
        }
    }

    pub inline fn wait(self: Semaphore) void {
        const rc = c.ove_sem_take(self.handle, WAIT_FOREVER);
        panicOnRc("Semaphore.wait", rc);
    }

    pub inline fn tryWait(self: Semaphore) bool {
        return tryRc("Semaphore.tryWait", c.ove_sem_take(self.handle, 0));
    }

    pub inline fn timedWait(self: Semaphore, d: Duration) WaitError!void {
        const rc = c.ove_sem_take(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Semaphore.timedWait", rc);
    }

    pub inline fn timedWaitUntil(self: Semaphore, deadline: Instant) WaitError!void {
        const t = time_mod.deadlineToTimeoutNs(deadline);
        const rc = c.ove_sem_take(self.handle, t);
        if (rc < 0) return mapTimeoutOnly("Semaphore.timedWaitUntil", rc);
    }

    pub inline fn post(self: Semaphore) void {
        c.ove_sem_give(self.handle);
    }
};

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

/// Binary event flag — auto-resets after a successful `wait()`.
pub const Event = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_event_t,
    storage: ?*c.ove_event_storage_t,

    /// Create a binary event in the unsignalled state.
    pub fn create(allocator: std.mem.Allocator) Error!Event {
        const storage = try allocator.create(c.ove_event_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_event_storage_t);
        var h: c.ove_event_t = null;
        try err.fromCode(c.ove_event_init(&h, storage));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: *Event) void {
        if (self.handle) |handle| {
            c.ove_event_deinit(handle);
            self.handle = null;
        }
        if (self.storage) |storage| {
            self.allocator.destroy(storage);
            self.storage = null;
        }
    }

    pub inline fn wait(self: Event) void {
        const rc = c.ove_event_wait(self.handle, WAIT_FOREVER);
        panicOnRc("Event.wait", rc);
    }

    pub inline fn tryWait(self: Event) bool {
        return tryRc("Event.tryWait", c.ove_event_wait(self.handle, 0));
    }

    pub inline fn timedWait(self: Event, d: Duration) WaitError!void {
        const rc = c.ove_event_wait(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Event.timedWait", rc);
    }

    pub inline fn timedWaitUntil(self: Event, deadline: Instant) WaitError!void {
        const t = time_mod.deadlineToTimeoutNs(deadline);
        const rc = c.ove_event_wait(self.handle, t);
        if (rc < 0) return mapTimeoutOnly("Event.timedWaitUntil", rc);
    }

    /// Signal the event from task context.
    pub inline fn signal(self: Event) void {
        c.ove_event_signal(self.handle);
    }

    /// Signal the event from an ISR.  No blocking, no locking;
    /// callable from any interrupt priority the substrate allows.
    pub inline fn signalFromIsr(self: Event) void {
        c.ove_event_signal_from_isr(self.handle);
    }
};

// ---------------------------------------------------------------------------
// CondVar
// ---------------------------------------------------------------------------

/// Condition variable paired with a `Mutex` for producer/consumer patterns.
pub const CondVar = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_condvar_t,
    storage: ?*c.ove_condvar_storage_t,

    /// Create a condition variable.  Pair with a [`Mutex`] when
    /// waiting via `wait`/`timedWait`/`waitWhileUntil`.
    pub fn create(allocator: std.mem.Allocator) Error!CondVar {
        const storage = try allocator.create(c.ove_condvar_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_condvar_storage_t);
        var h: c.ove_condvar_t = null;
        try err.fromCode(c.ove_condvar_init(&h, storage));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: *CondVar) void {
        if (self.handle) |handle| {
            c.ove_condvar_deinit(handle);
            self.handle = null;
        }
        if (self.storage) |storage| {
            self.allocator.destroy(storage);
            self.storage = null;
        }
    }

    pub inline fn wait(self: CondVar, mutex: Mutex) void {
        const rc = c.ove_condvar_wait(self.handle, mutex.handle, WAIT_FOREVER);
        panicOnRc("CondVar.wait", rc);
    }

    pub inline fn timedWait(self: CondVar, mutex: Mutex, d: Duration) WaitError!void {
        const rc = c.ove_condvar_wait(self.handle, mutex.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("CondVar.timedWait", rc);
    }

    pub inline fn timedWaitUntil(self: CondVar, mutex: Mutex, deadline: Instant) WaitError!void {
        const t = time_mod.deadlineToTimeoutNs(deadline);
        const rc = c.ove_condvar_wait(self.handle, mutex.handle, t);
        if (rc < 0) return mapTimeoutOnly("CondVar.timedWaitUntil", rc);
    }

    /// Predicate-driven deadline wait — spurious-wakeup-safe.
    pub fn waitWhileUntil(
        self: CondVar,
        mutex: Mutex,
        deadline: Instant,
        comptime pred: anytype,
        args: anytype,
    ) WaitError!void {
        while (@call(.auto, pred, args)) {
            if (Instant.now().ns >= deadline.ns) return error.Timeout;
            try self.timedWaitUntil(mutex, deadline);
        }
    }

    /// Wake one waiter on this condition variable.
    pub inline fn signal(self: CondVar) void {
        c.ove_condvar_signal(self.handle);
    }

    /// Wake all waiters on this condition variable.
    pub inline fn broadcast(self: CondVar) void {
        c.ove_condvar_broadcast(self.handle);
    }
};
