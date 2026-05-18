// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Synchronisation primitives.
//!
//! Each primitive has a per-mode shape selected at comptime:
//!
//! ## Heap mode (`CONFIG_OVE_ZERO_HEAP` not set)
//!
//! Returned by value from `create()`.  Fully movable — no embedded storage,
//! no pin tracker.  Methods receive `Self` by value (cheap; the wrapper is
//! one pointer).
//!
//! ```zig
//! var mtx = try ove.Mutex.create();
//! defer mtx.deinit();
//! try mtx.lock(ove.wait_forever);
//! mtx.unlock();
//! ```
//!
//! ## Zero-heap mode (`CONFIG_OVE_ZERO_HEAP` set)
//!
//! Embeds kernel-object storage as a struct field.  Two-phase init; the
//! wrapper **must not be moved, copied, passed by value, or relocated**
//! after `init()` — the kernel handle stored in `self.handle` references
//! `&self.storage`.  Debug builds (`std.debug.runtime_safety == true`)
//! record `&self` at `init()` and panic if any method later sees a
//! different address.  Release builds compile the check out at zero cost.
//!
//! `init()` failure leaves `self` in its original `undefined` state — do
//! not register `defer self.deinit()` until after `try self.init()`
//! succeeds.
//!
//! ```zig
//! var mtx: ove.Mutex = undefined;
//! try mtx.init();
//! defer mtx.deinit();
//! try mtx.lock(ove.wait_forever);
//! mtx.unlock();
//! ```

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

// ---------------------------------------------------------------------------
// Per-operation narrow error sets (A3).
//
// Each blocking wait can only return `error.Timeout` at runtime — every
// other substrate code (`OVE_ERR_INVALID_PARAM`, etc.) is a programming
// bug here (handle is null, subsystem deinit'd, …) and panics at the
// FFI boundary via `panicUnexpected`.  Callers `switch (e) { error.Timeout
// => …, }` exhaustively without an `else` arm.
//
// Initialisation (`create` / `init`) still returns the global `Error`
// because the substrate failure space at construction is genuinely wide
// (NoMemory, NotRegistered, NotSupported, …).
// ---------------------------------------------------------------------------

/// Error set for `Mutex.lock*` / `RecursiveMutex.lock*` / `acquire*`.
pub const LockError = error{Timeout};

/// Error set for `Semaphore.wait*` / `Event.wait*` / `CondVar.wait*`.
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

/// `tryX()` helper: the substrate is called with timeout=0; OK → true,
/// Timeout → false; any other code is a programming bug → panic.
inline fn tryRc(comptime ctx: []const u8, rc: c_int) bool {
    return switch (rc) {
        0 => true,
        c.OVE_ERR_TIMEOUT => false,
        else => panicUnexpected(ctx, rc),
    };
}

/// Forever-blocking helper: under WAIT_FOREVER, the substrate only fails
/// on programming bugs (handle null, subsystem deinit'd) — panic.
inline fn panicOnRc(comptime ctx: []const u8, rc: c_int) void {
    if (rc < 0) panicUnexpected(ctx, rc);
}

const Duration = @import("time.zig").Duration;
const WAIT_FOREVER = c.OVE_WAIT_FOREVER;

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

/// Non-recursive mutual exclusion lock.
pub const Mutex = if (pin.zero_heap) ZeroHeapMutex else HeapMutex;

const HeapMutex = struct {
    handle: c.ove_mutex_t,

    pub fn create() Error!Mutex {
        var h: c.ove_mutex_t = null;
        try err.fromCode(c.ove_mutex_create(&h));
        return .{ .handle = h };
    }

    pub fn deinit(self: Mutex) void {
        if (self.handle == null) return;
        c.ove_mutex_destroy(self.handle);
    }

    /// Acquire the lock, blocking indefinitely.  Infallible after a
    /// successful `create()` — the substrate's WAIT_FOREVER path only
    /// reports programming-bug codes, which panic at the FFI boundary.
    /// `std.Thread.Mutex.lock` mirror.
    pub inline fn lock(self: Mutex) void {
        const rc = c.ove_mutex_lock(self.handle, WAIT_FOREVER);
        panicOnRc("Mutex.lock", rc);
    }

    /// Non-blocking lock attempt — returns `true` on success.
    /// `std.Thread.Mutex.tryLock` mirror.
    pub inline fn tryLock(self: Mutex) bool {
        return tryRc("Mutex.tryLock", c.ove_mutex_lock(self.handle, 0));
    }

    /// Lock with a bounded duration.
    pub inline fn lockFor(self: Mutex, d: Duration) LockError!void {
        const rc = c.ove_mutex_lock(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Mutex.lockFor", rc);
    }

    pub inline fn unlock(self: Mutex) void {
        c.ove_mutex_unlock(self.handle);
    }

    /// RAII guard returned by `acquire*()`.  Holds the mutex until
    /// `release()` is called (typically via `defer`).
    pub const Guard = struct {
        mutex: Mutex,
        pub fn release(self: Guard) void {
            self.mutex.unlock();
        }
    };

    /// Forever-blocking acquire — returns a guard.  Infallible.
    ///
    /// ```zig
    /// const g = mtx.acquire();
    /// defer g.release();
    /// ```
    pub fn acquire(self: Mutex) Guard {
        self.lock();
        return .{ .mutex = self };
    }

    /// Bounded-duration acquire.
    pub fn acquireFor(self: Mutex, d: Duration) LockError!Guard {
        try self.lockFor(d);
        return .{ .mutex = self };
    }
};

const ZeroHeapMutex = struct {
    storage: c.ove_mutex_storage_t,
    handle: c.ove_mutex_t,
    tracker: pin.Tracker,

    /// Attach a kernel mutex to this wrapper.  Must be called exactly once
    /// before any locking operation, and `self` must outlive every
    /// subsequent call (the kernel handle points into `self.storage`).
    pub fn init(self: *Mutex) Error!void {
        self.storage = std.mem.zeroes(c.ove_mutex_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_mutex_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    /// Release kernel resources.  Idempotent.  Must be called from the
    /// same address as `init()`.
    pub fn deinit(self: *Mutex) void {
        self.tracker.assertSame(self, "ove.Mutex");
        if (self.handle == null) return;
        c.ove_mutex_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub inline fn lock(self: *Mutex) void {
        self.tracker.assertSame(self, "ove.Mutex");
        const rc = c.ove_mutex_lock(self.handle, WAIT_FOREVER);
        panicOnRc("Mutex.lock", rc);
    }

    pub inline fn tryLock(self: *Mutex) bool {
        self.tracker.assertSame(self, "ove.Mutex");
        return tryRc("Mutex.tryLock", c.ove_mutex_lock(self.handle, 0));
    }

    pub inline fn lockFor(self: *Mutex, d: Duration) LockError!void {
        self.tracker.assertSame(self, "ove.Mutex");
        const rc = c.ove_mutex_lock(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Mutex.lockFor", rc);
    }

    pub inline fn unlock(self: *Mutex) void {
        self.tracker.assertSame(self, "ove.Mutex");
        c.ove_mutex_unlock(self.handle);
    }

    pub const Guard = struct {
        mutex: *Mutex,
        pub fn release(self: Guard) void {
            self.mutex.unlock();
        }
    };

    pub fn acquire(self: *Mutex) Guard {
        self.lock();
        return .{ .mutex = self };
    }

    pub fn acquireFor(self: *Mutex, d: Duration) LockError!Guard {
        try self.lockFor(d);
        return .{ .mutex = self };
    }
};

// ---------------------------------------------------------------------------
// RecursiveMutex
// ---------------------------------------------------------------------------

/// Recursive mutual exclusion lock.  The same task may lock multiple times;
/// each `lock()` must be paired with `unlock()`.
pub const RecursiveMutex = if (pin.zero_heap) ZeroHeapRecursiveMutex else HeapRecursiveMutex;

const HeapRecursiveMutex = struct {
    handle: c.ove_mutex_t,

    pub fn create() Error!RecursiveMutex {
        var h: c.ove_mutex_t = null;
        try err.fromCode(c.ove_recursive_mutex_create(&h));
        return .{ .handle = h };
    }

    pub fn deinit(self: RecursiveMutex) void {
        if (self.handle == null) return;
        c.ove_mutex_destroy(self.handle);
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
};

const ZeroHeapRecursiveMutex = struct {
    storage: c.ove_mutex_storage_t,
    handle: c.ove_mutex_t,
    tracker: pin.Tracker,

    pub fn init(self: *RecursiveMutex) Error!void {
        self.storage = std.mem.zeroes(c.ove_mutex_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_recursive_mutex_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    pub fn deinit(self: *RecursiveMutex) void {
        self.tracker.assertSame(self, "ove.RecursiveMutex");
        if (self.handle == null) return;
        c.ove_mutex_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub inline fn lock(self: *RecursiveMutex) void {
        self.tracker.assertSame(self, "ove.RecursiveMutex");
        const rc = c.ove_recursive_mutex_lock(self.handle, WAIT_FOREVER);
        panicOnRc("RecursiveMutex.lock", rc);
    }

    pub inline fn tryLock(self: *RecursiveMutex) bool {
        self.tracker.assertSame(self, "ove.RecursiveMutex");
        return tryRc("RecursiveMutex.tryLock", c.ove_recursive_mutex_lock(self.handle, 0));
    }

    pub inline fn lockFor(self: *RecursiveMutex, d: Duration) LockError!void {
        self.tracker.assertSame(self, "ove.RecursiveMutex");
        const rc = c.ove_recursive_mutex_lock(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("RecursiveMutex.lockFor", rc);
    }

    pub inline fn unlock(self: *RecursiveMutex) void {
        self.tracker.assertSame(self, "ove.RecursiveMutex");
        c.ove_recursive_mutex_unlock(self.handle);
    }

    pub const Guard = struct {
        mutex: *RecursiveMutex,
        pub fn release(self: Guard) void {
            self.mutex.unlock();
        }
    };

    pub fn acquire(self: *RecursiveMutex) Guard {
        self.lock();
        return .{ .mutex = self };
    }

    pub fn acquireFor(self: *RecursiveMutex, d: Duration) LockError!Guard {
        try self.lockFor(d);
        return .{ .mutex = self };
    }
};

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

/// Counting semaphore for signalling between tasks or from ISRs.
pub const Semaphore = if (pin.zero_heap) ZeroHeapSemaphore else HeapSemaphore;

const HeapSemaphore = struct {
    handle: c.ove_sem_t,

    pub fn create(initial: u32, max: u32) Error!Semaphore {
        var h: c.ove_sem_t = null;
        try err.fromCode(c.ove_sem_create(&h, initial, max));
        return .{ .handle = h };
    }

    pub fn deinit(self: Semaphore) void {
        if (self.handle == null) return;
        c.ove_sem_destroy(self.handle);
    }

    /// Decrement (acquire) the semaphore, blocking indefinitely.
    /// `std.Thread.Semaphore.wait` mirror.  Infallible.
    pub inline fn wait(self: Semaphore) void {
        const rc = c.ove_sem_take(self.handle, WAIT_FOREVER);
        panicOnRc("Semaphore.wait", rc);
    }

    /// Non-blocking decrement attempt — returns `true` on success.
    pub inline fn tryWait(self: Semaphore) bool {
        return tryRc("Semaphore.tryWait", c.ove_sem_take(self.handle, 0));
    }

    /// Decrement with a bounded duration.
    /// `std.Thread.Semaphore.timedWait` mirror.
    pub inline fn timedWait(self: Semaphore, d: Duration) WaitError!void {
        const rc = c.ove_sem_take(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Semaphore.timedWait", rc);
    }

    /// Increment (release) the semaphore.  `std.Thread.Semaphore.post`
    /// mirror.
    pub inline fn post(self: Semaphore) void {
        c.ove_sem_give(self.handle);
    }
};

const ZeroHeapSemaphore = struct {
    storage: c.ove_sem_storage_t,
    handle: c.ove_sem_t,
    tracker: pin.Tracker,

    pub fn init(self: *Semaphore, initial: u32, max: u32) Error!void {
        self.storage = std.mem.zeroes(c.ove_sem_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_sem_init(&self.handle, &self.storage, initial, max));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Semaphore) void {
        self.tracker.assertSame(self, "ove.Semaphore");
        if (self.handle == null) return;
        c.ove_sem_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub inline fn wait(self: *Semaphore) void {
        self.tracker.assertSame(self, "ove.Semaphore");
        const rc = c.ove_sem_take(self.handle, WAIT_FOREVER);
        panicOnRc("Semaphore.wait", rc);
    }

    pub inline fn tryWait(self: *Semaphore) bool {
        self.tracker.assertSame(self, "ove.Semaphore");
        return tryRc("Semaphore.tryWait", c.ove_sem_take(self.handle, 0));
    }

    pub inline fn timedWait(self: *Semaphore, d: Duration) WaitError!void {
        self.tracker.assertSame(self, "ove.Semaphore");
        const rc = c.ove_sem_take(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Semaphore.timedWait", rc);
    }

    pub inline fn post(self: *Semaphore) void {
        self.tracker.assertSame(self, "ove.Semaphore");
        c.ove_sem_give(self.handle);
    }
};

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

/// Binary event flag — auto-resets after a successful `wait()`.
pub const Event = if (pin.zero_heap) ZeroHeapEvent else HeapEvent;

const HeapEvent = struct {
    handle: c.ove_event_t,

    pub fn create() Error!Event {
        var h: c.ove_event_t = null;
        try err.fromCode(c.ove_event_create(&h));
        return .{ .handle = h };
    }

    pub fn deinit(self: Event) void {
        if (self.handle == null) return;
        c.ove_event_destroy(self.handle);
    }

    /// Block indefinitely until signalled.  Infallible.
    pub inline fn wait(self: Event) void {
        const rc = c.ove_event_wait(self.handle, WAIT_FOREVER);
        panicOnRc("Event.wait", rc);
    }

    /// Non-blocking check — returns `true` if signal pending, `false`
    /// otherwise.  Auto-resets on success.
    pub inline fn tryWait(self: Event) bool {
        return tryRc("Event.tryWait", c.ove_event_wait(self.handle, 0));
    }

    /// Block up to `d` for the signal.
    pub inline fn timedWait(self: Event, d: Duration) WaitError!void {
        const rc = c.ove_event_wait(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Event.timedWait", rc);
    }

    pub inline fn signal(self: Event) void {
        c.ove_event_signal(self.handle);
    }

    pub inline fn signalFromIsr(self: Event) void {
        c.ove_event_signal_from_isr(self.handle);
    }
};

const ZeroHeapEvent = struct {
    storage: c.ove_event_storage_t,
    handle: c.ove_event_t,
    tracker: pin.Tracker,

    pub fn init(self: *Event) Error!void {
        self.storage = std.mem.zeroes(c.ove_event_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_event_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Event) void {
        self.tracker.assertSame(self, "ove.Event");
        if (self.handle == null) return;
        c.ove_event_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub inline fn wait(self: *Event) void {
        self.tracker.assertSame(self, "ove.Event");
        const rc = c.ove_event_wait(self.handle, WAIT_FOREVER);
        panicOnRc("Event.wait", rc);
    }

    pub inline fn tryWait(self: *Event) bool {
        self.tracker.assertSame(self, "ove.Event");
        return tryRc("Event.tryWait", c.ove_event_wait(self.handle, 0));
    }

    pub inline fn timedWait(self: *Event, d: Duration) WaitError!void {
        self.tracker.assertSame(self, "ove.Event");
        const rc = c.ove_event_wait(self.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("Event.timedWait", rc);
    }

    pub inline fn signal(self: *Event) void {
        self.tracker.assertSame(self, "ove.Event");
        c.ove_event_signal(self.handle);
    }

    pub inline fn signalFromIsr(self: *Event) void {
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
pub const CondVar = if (pin.zero_heap) ZeroHeapCondVar else HeapCondVar;

const HeapCondVar = struct {
    handle: c.ove_condvar_t,

    pub fn create() Error!CondVar {
        var h: c.ove_condvar_t = null;
        try err.fromCode(c.ove_condvar_create(&h));
        return .{ .handle = h };
    }

    pub fn deinit(self: CondVar) void {
        if (self.handle == null) return;
        c.ove_condvar_destroy(self.handle);
    }

    /// Atomically release `mutex` and wait indefinitely for a
    /// signal/broadcast.  Re-acquires `mutex` before returning.
    /// Infallible.  `std.Thread.Condition.wait` mirror.
    pub inline fn wait(self: CondVar, mutex: Mutex) void {
        const rc = c.ove_condvar_wait(self.handle, mutex.handle, WAIT_FOREVER);
        panicOnRc("CondVar.wait", rc);
    }

    /// Atomically release `mutex` and wait up to `d` for a signal.
    /// `std.Thread.Condition.timedWait` mirror.
    pub inline fn timedWait(self: CondVar, mutex: Mutex, d: Duration) WaitError!void {
        const rc = c.ove_condvar_wait(self.handle, mutex.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("CondVar.timedWait", rc);
    }

    pub inline fn signal(self: CondVar) void {
        c.ove_condvar_signal(self.handle);
    }

    pub inline fn broadcast(self: CondVar) void {
        c.ove_condvar_broadcast(self.handle);
    }
};

const ZeroHeapCondVar = struct {
    storage: c.ove_condvar_storage_t,
    handle: c.ove_condvar_t,
    tracker: pin.Tracker,

    pub fn init(self: *CondVar) Error!void {
        self.storage = std.mem.zeroes(c.ove_condvar_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_condvar_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    pub fn deinit(self: *CondVar) void {
        self.tracker.assertSame(self, "ove.CondVar");
        if (self.handle == null) return;
        c.ove_condvar_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub inline fn wait(self: *CondVar, mutex: *Mutex) void {
        self.tracker.assertSame(self, "ove.CondVar");
        const rc = c.ove_condvar_wait(self.handle, mutex.handle, WAIT_FOREVER);
        panicOnRc("CondVar.wait", rc);
    }

    pub inline fn timedWait(self: *CondVar, mutex: *Mutex, d: Duration) WaitError!void {
        self.tracker.assertSame(self, "ove.CondVar");
        const rc = c.ove_condvar_wait(self.handle, mutex.handle, d.ns);
        if (rc < 0) return mapTimeoutOnly("CondVar.timedWait", rc);
    }

    pub inline fn signal(self: *CondVar) void {
        self.tracker.assertSame(self, "ove.CondVar");
        c.ove_condvar_signal(self.handle);
    }

    pub inline fn broadcast(self: *CondVar) void {
        self.tracker.assertSame(self, "ove.CondVar");
        c.ove_condvar_broadcast(self.handle);
    }
};
