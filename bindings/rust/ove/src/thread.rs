// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! RTOS thread management.
//!
//! The API splits the thread surface into three types so ownership,
//! observation, and configuration each have a clear home:
//!
//!   - [`Thread`] is a **non-owning** handle.  Returned by
//!     [`Thread::current`] and by [`JoinHandle::thread`].  Carries the
//!     read-only and signal operations (`get_state`,
//!     `get_stack_usage`, `set_priority`, `suspend`/`resume`,
//!     `request_stop`, …).  Dropping a `Thread` value does nothing.
//!   - [`JoinHandle`] is the **owning** type returned by
//!     [`Builder::spawn`] (and its `spawn_cooperative`/`spawn_simple`/
//!     `spawn_static` siblings).  On drop it calls
//!     [`JoinHandle::request_stop`] and waits for the worker to finish.
//!     Use [`JoinHandle::detach`] to opt out of the join-on-drop.
//!   - [`Builder`] is the only way to spawn a new thread.  Cooperative
//!     cancellation is opt-in via the closure signature
//!     (`FnOnce(StopToken)` vs `fn()`).
//!
//! Spawning a cooperative worker:
//! ```ignore
//! let h = Thread::builder()
//!     .name(c"worker")
//!     .priority(Priority::Normal)
//!     .stack_size(4096)
//!     .spawn(|tok: StopToken| {
//!         while !tok.is_stopped() { /* work */ }
//!     })?;
//! // h goes out of scope -> request_stop + join, no deadlock.
//! ```
//!
//! Spawning a self-terminating one-shot (no token):
//! ```ignore
//! fn entry() { /* one-shot work */ }
//! let _h = Thread::builder()
//!     .name(c"oneshot")
//!     .priority(Priority::Normal)
//!     .stack_size(4096)
//!     .spawn_simple(entry)?;
//! ```

use core::fmt;
use core::marker::PhantomData;

use crate::bindings;
use crate::error::{Error, Result};

// Round-tripping `fn()` / `fn(StopToken)` through `*mut c_void`
// requires the two to be the same size.  Holds on every target
// oveRTOS supports today — ARM Cortex-M (32-bit thumb), x86_64,
// RISC-V (32/64), and WebAssembly.  If a future port lands on a
// target where function pointers are wider than data pointers (some
// segmented or capability architectures), this assertion fires at
// compile time so the silent corruption mode becomes a build failure.
const _: () = assert!(
    core::mem::size_of::<fn()>() == core::mem::size_of::<*mut core::ffi::c_void>(),
    "ove::thread: fn() and *mut c_void must be the same size for FFI round-trip"
);
const _: () = assert!(
    core::mem::size_of::<fn(StopToken)>() == core::mem::size_of::<*mut core::ffi::c_void>(),
    "ove::thread: fn(StopToken) and *mut c_void must be the same size for FFI round-trip"
);

/// Thread priority levels, matching `ove_prio_t`.
///
/// Variants are ordered from lowest (`Idle`) to highest (`Critical`) so that
/// standard comparison operators (`<`, `>`) work intuitively.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u32)]
pub enum Priority {
    /// Lowest priority, typically the RTOS idle task level.
    Idle = 0,
    /// Low background priority.
    Low = 1,
    /// Below-normal priority.
    BelowNormal = 2,
    /// Default priority for most application threads.
    Normal = 3,
    /// Above-normal priority for time-sensitive work.
    AboveNormal = 4,
    /// High priority for latency-critical tasks.
    High = 5,
    /// Real-time priority; preempts most other threads.
    Realtime = 6,
    /// Highest priority; reserved for system-critical tasks.
    Critical = 7,
}

/// Thread state, matching `ove_thread_state_t`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ThreadState {
    /// The thread is currently executing on a CPU core.
    Running,
    /// The thread is ready to run but waiting for a CPU slot.
    Ready,
    /// The thread is waiting on a synchronization primitive or I/O.
    Blocked,
    /// The thread has been explicitly suspended via [`Thread::suspend`].
    Suspended,
    /// The thread has finished executing.
    Terminated,
    /// The state reported by the RTOS does not match any known variant.
    Unknown,
}

/// Runtime statistics for a thread.
#[derive(Debug, Clone, Copy)]
pub struct ThreadStats {
    /// Total CPU time consumed by this thread in microseconds.
    pub runtime_us: u64,
    /// CPU utilization as a percentage multiplied by 100 (e.g. 5025 = 50.25%).
    pub cpu_percent_x100: u32,
}

// ---------------------------------------------------------------------------
// StopToken — read-only handle to the per-thread cancellation flag.
// ---------------------------------------------------------------------------

/// Read-only handle to a thread's cooperative-cancellation flag.
///
/// Cheap to copy, `Send + Sync`.  Reads the per-thread atomic flag set
/// by [`JoinHandle::request_stop`] (or implicitly by [`JoinHandle`]'s
/// [`Drop`]).
///
/// Workers spawned via [`Builder::spawn`] receive a `StopToken` as
/// their entry argument and poll [`StopToken::is_stopped`] to break
/// cleanly out of their loop.  Tokens can be cloned and passed to
/// helper functions that need to bail out without being handed the
/// owning [`JoinHandle`].
///
/// ```ignore
/// use ove::{Thread, StopToken, Priority};
///
/// let h = Thread::builder()
///     .name(c"worker")
///     .priority(Priority::Normal)
///     .stack_size(4096)
///     .spawn(|tok: StopToken| {
///         while !tok.is_stopped() {
///             // do work
///         }
///     })?;
///
/// // h goes out of scope -> Drop calls request_stop + join.
/// ```
#[derive(Debug, Clone, Copy)]
pub struct StopToken {
    handle: bindings::ove_thread_t,
}

// SAFETY: the handle is an opaque RTOS pointer.  Access through it
// goes via __atomic_* primitives in the substrate; the handle itself
// is shareable across threads.
unsafe impl Send for StopToken {}
unsafe impl Sync for StopToken {}

impl StopToken {
    /// Construct an empty token that never signals stop.  Useful as a
    /// default for fields filled in later.
    #[inline]
    pub const fn empty() -> Self {
        Self {
            handle: core::ptr::null_mut(),
        }
    }

    /// `true` if [`JoinHandle::request_stop`] (or the parent's [`Drop`])
    /// has been called on the referenced thread.  Returns `false` for
    /// an empty token.
    #[inline]
    pub fn is_stopped(&self) -> bool {
        !self.handle.is_null() && unsafe { bindings::ove_thread_should_stop(self.handle) }
    }

    /// `true` if this token references a real thread (vs.
    /// [`StopToken::empty`]).
    #[inline]
    pub fn stop_possible(&self) -> bool {
        !self.handle.is_null()
    }

    /// Raw handle accessor for advanced use.
    #[inline]
    pub fn raw_handle(&self) -> bindings::ove_thread_t {
        self.handle
    }
}

// ---------------------------------------------------------------------------
// Internal: trampoline closures + RAII guards for FnOnce capture boxes.
// ---------------------------------------------------------------------------

#[cfg(all(not(zero_heap), feature = "alloc"))]
type ClosureBox = alloc::boxed::Box<dyn FnOnce(StopToken) + Send + 'static>;

#[cfg(all(not(zero_heap), feature = "alloc"))]
struct ThunkGuard {
    raw: *mut ClosureBox,
}

#[cfg(all(not(zero_heap), feature = "alloc"))]
impl ThunkGuard {
    fn forget(mut self) {
        self.raw = core::ptr::null_mut();
    }
}

#[cfg(all(not(zero_heap), feature = "alloc"))]
impl Drop for ThunkGuard {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            // SAFETY: forget() never ran, so the kernel did not adopt
            // the boxed closure (spawn failed before forget).  We still
            // own `raw`, free it now.
            let _ = unsafe { alloc::boxed::Box::from_raw(self.raw) };
        }
    }
}

/// `Thread::current()` and worker trampolines both need to wait for the
/// substrate to publish the per-thread "self" handle before constructing
/// a [`StopToken`] — on FreeRTOS the parent task-tag publish happens
/// AFTER `xTaskCreate` returns and equal-priority workers can outrace it.
#[inline]
unsafe fn wait_for_self() -> bindings::ove_thread_t {
    let mut handle = unsafe { bindings::ove_thread_get_self() };
    while handle.is_null() {
        unsafe { bindings::ove_thread_yield() };
        handle = unsafe { bindings::ove_thread_get_self() };
    }
    handle
}

unsafe extern "C" fn fn_simple_trampoline(arg: *mut core::ffi::c_void) {
    // SAFETY: `arg` was set by the caller from a `fn()` pointer.  On
    // supported targets `fn()` is pointer-sized and C-ABI-compatible.
    let entry: fn() = unsafe { core::mem::transmute(arg) };
    entry();
}

unsafe extern "C" fn fn_cooperative_trampoline(arg: *mut core::ffi::c_void) {
    let entry: fn(StopToken) = unsafe { core::mem::transmute(arg) };
    let handle = unsafe { wait_for_self() };
    entry(StopToken { handle });
}

#[cfg(all(not(zero_heap), feature = "alloc"))]
unsafe extern "C" fn box_cooperative_trampoline(arg: *mut core::ffi::c_void) {
    // SAFETY: re-takes ownership of the box minted in Builder::spawn.
    let boxed: alloc::boxed::Box<ClosureBox> =
        unsafe { alloc::boxed::Box::from_raw(arg as *mut ClosureBox) };
    let handle = unsafe { wait_for_self() };
    (*boxed)(StopToken { handle });
}

// ---------------------------------------------------------------------------
// Thread — non-owning handle (analogous to std::thread::Thread).
// ---------------------------------------------------------------------------

/// Non-owning handle to an RTOS thread.
///
/// Returned by [`Thread::current`] and [`JoinHandle::thread`].  Dropping
/// a `Thread` does **not** destroy the underlying RTOS thread — use
/// [`JoinHandle`] for that.  Carries the read-only and signal-only
/// operations (`get_state`, `suspend`/`resume`, `request_stop`, …).
#[derive(Clone, Copy)]
pub struct Thread {
    handle: bindings::ove_thread_t,
}

// SAFETY: RTOS thread handles are shareable across threads once created.
// Access to the handle is synchronized by the RTOS itself.
unsafe impl Send for Thread {}
unsafe impl Sync for Thread {}

impl Thread {
    /// Sleep the current thread for `ms` milliseconds.
    #[inline]
    pub fn sleep_ms(ms: u32) {
        unsafe { bindings::ove_thread_sleep_ms(ms) }
    }

    /// Yield the current thread's time slice.
    #[inline]
    pub fn yield_now() {
        unsafe { bindings::ove_thread_yield() }
    }

    /// Get a non-owning handle to the currently running thread.
    #[inline]
    pub fn current() -> Self {
        let handle = unsafe { bindings::ove_thread_get_self() };
        Self { handle }
    }

    /// Begin spawning a new thread.  Returns a fluent [`Builder`].
    ///
    /// ```ignore
    /// let h = Thread::builder()
    ///     .name(c"worker")
    ///     .priority(Priority::Normal)
    ///     .stack_size(4096)
    ///     .spawn(|tok| { while !tok.is_stopped() { /* work */ } })?;
    /// ```
    #[inline]
    pub fn builder() -> Builder {
        Builder::new()
    }

    /// Suspend this thread.
    #[inline]
    pub fn suspend(&self) {
        unsafe { bindings::ove_thread_suspend(self.handle) }
    }

    /// Resume this thread.
    #[inline]
    pub fn resume(&self) {
        unsafe { bindings::ove_thread_resume(self.handle) }
    }

    /// Set this thread's priority.
    #[inline]
    pub fn set_priority(&self, prio: Priority) {
        unsafe { bindings::ove_thread_set_priority(self.handle, prio as bindings::ove_prio_t) }
    }

    /// Get current stack usage in bytes.
    #[inline]
    pub fn get_stack_usage(&self) -> usize {
        unsafe { bindings::ove_thread_get_stack_usage(self.handle) }
    }

    /// Get the thread's current state.
    pub fn get_state(&self) -> ThreadState {
        let state = unsafe { bindings::ove_thread_get_state(self.handle) };
        match state {
            bindings::OVE_THREAD_STATE_RUNNING => ThreadState::Running,
            bindings::OVE_THREAD_STATE_READY => ThreadState::Ready,
            bindings::OVE_THREAD_STATE_BLOCKED => ThreadState::Blocked,
            bindings::OVE_THREAD_STATE_SUSPENDED => ThreadState::Suspended,
            bindings::OVE_THREAD_STATE_TERMINATED => ThreadState::Terminated,
            _ => ThreadState::Unknown,
        }
    }

    /// Get runtime statistics (CPU time and utilization) for this thread.
    ///
    /// # Errors
    /// Returns an error if the RTOS does not support runtime statistics or the
    /// thread handle is invalid.
    pub fn get_runtime_stats(&self) -> Result<ThreadStats> {
        let mut stats = bindings::ove_thread_stats {
            runtime_us: 0,
            cpu_percent_x100: 0,
        };
        let rc = unsafe { bindings::ove_thread_get_runtime_stats(self.handle, &mut stats) };
        Error::from_code(rc)?;
        Ok(ThreadStats {
            runtime_us: stats.runtime_us,
            cpu_percent_x100: stats.cpu_percent_x100,
        })
    }

    /// Request the thread to stop cooperatively.
    ///
    /// Sets the per-thread atomic cancellation flag.  The worker must
    /// poll [`StopToken::is_stopped`] (via the token it received at
    /// spawn time) for this to have any effect — the substrate does
    /// NOT force-terminate.  Safe from any context (ISR, other thread,
    /// the thread itself).  Idempotent; the flag is sticky.
    #[inline]
    pub fn request_stop(&self) {
        if !self.handle.is_null() {
            unsafe { bindings::ove_thread_request_stop(self.handle) };
        }
    }

    /// Get a [`StopToken`] referencing this thread's cancellation flag.
    #[inline]
    pub fn stop_token(&self) -> StopToken {
        StopToken {
            handle: self.handle,
        }
    }

    /// `true` if [`Thread::request_stop`] has been called on this thread.
    #[inline]
    pub fn stop_requested(&self) -> bool {
        !self.handle.is_null() && unsafe { bindings::ove_thread_should_stop(self.handle) }
    }

    /// Raw handle accessor for advanced use.
    #[inline]
    pub fn raw_handle(&self) -> bindings::ove_thread_t {
        self.handle
    }
}

impl fmt::Debug for Thread {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Thread")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish()
    }
}

// ---------------------------------------------------------------------------
// Builder — fluent spawn configuration.
// ---------------------------------------------------------------------------

/// Fluent thread-spawn configuration.  Construct via [`Thread::builder`].
///
/// Default-constructable so chains like
/// `Builder::new().name(c"foo").spawn(...)` also work.  Required-field
/// validation happens at [`spawn`](Builder::spawn) time; missing name
/// uses an empty default which most RTOSes accept.
pub struct Builder {
    name: &'static core::ffi::CStr,
    priority: Priority,
    stack_size: usize,
}

impl Default for Builder {
    fn default() -> Self {
        Self::new()
    }
}

impl Builder {
    /// Create a new builder with default settings: no name, normal
    /// priority, 4 KB stack.
    #[inline]
    pub const fn new() -> Self {
        Self {
            // SAFETY: empty literal — `c""` requires Rust 1.77 which
            // we already require; keep as const.
            name: c"",
            priority: Priority::Normal,
            stack_size: 4096,
        }
    }

    /// Set the thread name.  Use a `c"..."` literal (Rust 1.77+) to
    /// guarantee null termination at compile time.
    #[inline]
    pub fn name(mut self, name: &'static core::ffi::CStr) -> Self {
        self.name = name;
        self
    }

    /// Set the thread priority.
    #[inline]
    pub fn priority(mut self, priority: Priority) -> Self {
        self.priority = priority;
        self
    }

    /// Set the stack size in bytes.
    #[inline]
    pub fn stack_size(mut self, stack_size: usize) -> Self {
        self.stack_size = stack_size;
        self
    }

    /// Spawn a thread with a `FnOnce(StopToken)` closure.
    ///
    /// Cooperative cancellation is built in: when the returned
    /// [`JoinHandle`] goes out of scope, [`Drop`] calls
    /// [`JoinHandle::request_stop`] before waiting for the worker so a
    /// `while !tok.is_stopped()` loop exits cleanly.  Use
    /// [`JoinHandle::detach`] to opt out of the join-on-drop.
    ///
    /// The closure is heap-allocated; requires the `alloc` feature and
    /// a registered `#[global_allocator]`.
    ///
    /// # Errors
    /// Returns [`Error::NoMemory`] on allocation failure or another
    /// error if the RTOS rejects the descriptor.
    #[cfg(all(not(zero_heap), feature = "alloc"))]
    pub fn spawn<F>(self, f: F) -> Result<JoinHandle<()>>
    where
        F: FnOnce(StopToken) + Send + 'static,
    {
        let boxed: ClosureBox = alloc::boxed::Box::new(f);
        let outer = alloc::boxed::Box::new(boxed);
        let raw = alloc::boxed::Box::into_raw(outer);
        let guard = ThunkGuard { raw };

        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_create(
                &mut handle,
                self.name.as_ptr() as *const _,
                Some(box_cooperative_trampoline),
                raw as *mut core::ffi::c_void,
                self.priority as bindings::ove_prio_t,
                self.stack_size,
            )
        };
        Error::from_code(rc)?;
        guard.forget();
        Ok(JoinHandle::new(handle))
    }

    /// Spawn a thread with a stateless `fn(StopToken)` entry.
    ///
    /// Heap-allocator-free variant of [`Builder::spawn`].  The entry
    /// pointer is round-tripped through `*mut c_void` (guarded by a
    /// compile-time size check).  No captures supported.
    ///
    /// # Errors
    /// See [`Builder::spawn`].
    #[cfg(not(zero_heap))]
    pub fn spawn_cooperative(self, entry: fn(StopToken)) -> Result<JoinHandle<()>> {
        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_create(
                &mut handle,
                self.name.as_ptr() as *const _,
                Some(fn_cooperative_trampoline),
                entry as *mut core::ffi::c_void,
                self.priority as bindings::ove_prio_t,
                self.stack_size,
            )
        };
        Error::from_code(rc)?;
        Ok(JoinHandle::new(handle))
    }

    /// Spawn a thread with a stateless `fn()` entry (no `StopToken`).
    ///
    /// Use when the worker is a self-terminating one-shot — it returns
    /// from its entry function on its own.  [`Drop`] on the returned
    /// [`JoinHandle`] still requests stop (no-op if the worker doesn't
    /// observe), then waits for the entry function to return.
    ///
    /// # Errors
    /// See [`Builder::spawn`].
    #[cfg(not(zero_heap))]
    pub fn spawn_simple(self, entry: fn()) -> Result<JoinHandle<()>> {
        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_create(
                &mut handle,
                self.name.as_ptr() as *const _,
                Some(fn_simple_trampoline),
                entry as *mut core::ffi::c_void,
                self.priority as bindings::ove_prio_t,
                self.stack_size,
            )
        };
        Error::from_code(rc)?;
        Ok(JoinHandle::new(handle))
    }

    /// Spawn a thread using caller-provided static storage and stack
    /// (zero-heap mode).
    ///
    /// # Safety
    /// - `storage` must outlive the returned [`JoinHandle`] (or until
    ///   [`JoinHandle::detach`] is called and the thread terminates).
    /// - `stack` must point to at least `stack_size` bytes of writable
    ///   memory with the same lifetime constraint.
    #[cfg(zero_heap)]
    pub unsafe fn spawn_static(
        self,
        storage: *mut bindings::ove_thread_storage_t,
        stack: *mut core::ffi::c_void,
        entry: fn(StopToken),
    ) -> Result<JoinHandle<()>> {
        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_init(
                &mut handle,
                storage,
                self.name.as_ptr() as *const _,
                Some(fn_cooperative_trampoline),
                entry as *mut core::ffi::c_void,
                self.priority as bindings::ove_prio_t,
                self.stack_size,
                stack,
            )
        };
        Error::from_code(rc)?;
        Ok(JoinHandle::new(handle))
    }

    /// Stateless `fn()` variant of [`Builder::spawn_static`] for legacy
    /// entries that don't accept a `StopToken`.
    ///
    /// # Safety
    /// See [`Builder::spawn_static`].
    #[cfg(zero_heap)]
    pub unsafe fn spawn_static_simple(
        self,
        storage: *mut bindings::ove_thread_storage_t,
        stack: *mut core::ffi::c_void,
        entry: fn(),
    ) -> Result<JoinHandle<()>> {
        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_init(
                &mut handle,
                storage,
                self.name.as_ptr() as *const _,
                Some(fn_simple_trampoline),
                entry as *mut core::ffi::c_void,
                self.priority as bindings::ove_prio_t,
                self.stack_size,
                stack,
            )
        };
        Error::from_code(rc)?;
        Ok(JoinHandle::new(handle))
    }
}

// ---------------------------------------------------------------------------
// JoinHandle — owning, joinable handle.
// ---------------------------------------------------------------------------

/// Owning handle to a spawned RTOS thread.
///
/// Drop semantics are cooperative-cancel + join (not detach):
///   - [`Drop`] calls [`request_stop`](Self::request_stop) then waits
///     for the worker to finish (the substrate's join wait).
///   - [`detach`](Self::detach) opts out of the join-on-drop — the
///     thread keeps running and the handle is consumed without
///     destroying the kernel object.
///
/// The `T` type parameter is reserved for future use (substrate doesn't
/// surface a worker's return value today); ignore it and use
/// `JoinHandle<()>`.
#[must_use = "dropping a JoinHandle requests stop and blocks until the worker exits"]
pub struct JoinHandle<T = ()> {
    handle: bindings::ove_thread_t,
    detached: bool,
    _phantom: PhantomData<fn() -> T>,
}

// SAFETY: same as Thread — the handle is RTOS-managed.
unsafe impl<T> Send for JoinHandle<T> {}
unsafe impl<T> Sync for JoinHandle<T> {}

impl<T> JoinHandle<T> {
    fn new(handle: bindings::ove_thread_t) -> Self {
        Self {
            handle,
            detached: false,
            _phantom: PhantomData,
        }
    }

    /// Get a non-owning [`Thread`] view of the running thread.  Use for
    /// signalling (`suspend`, `set_priority`, …) without giving away
    /// the owning handle.
    #[inline]
    pub fn thread(&self) -> Thread {
        Thread {
            handle: self.handle,
        }
    }

    /// Request the thread to stop cooperatively.  See [`Thread::request_stop`].
    #[inline]
    pub fn request_stop(&self) {
        if !self.handle.is_null() {
            unsafe { bindings::ove_thread_request_stop(self.handle) };
        }
    }

    /// Get a [`StopToken`] referencing this thread's cancellation flag.
    #[inline]
    pub fn stop_token(&self) -> StopToken {
        StopToken {
            handle: self.handle,
        }
    }

    /// `true` if [`request_stop`](Self::request_stop) has been called.
    #[inline]
    pub fn stop_requested(&self) -> bool {
        !self.handle.is_null() && unsafe { bindings::ove_thread_should_stop(self.handle) }
    }

    /// Wait for the worker to finish without requesting a stop first.
    ///
    /// Returns once the worker's entry function returns and the
    /// substrate has joined.  For workers that loop forever without
    /// observing [`StopToken`] this blocks indefinitely — call
    /// [`request_stop`](Self::request_stop) beforehand if you need a
    /// cooperative shutdown.
    ///
    /// `T` is always `()` today; the substrate doesn't surface worker
    /// return values.  The method signature reserves the parameter for
    /// a future expansion (matches `std::thread::JoinHandle::join`).
    pub fn join(mut self) -> Result<T> {
        if self.handle.is_null() {
            return Err(Error::InvalidParam);
        }
        let handle = self.handle;
        self.handle = core::ptr::null_mut();
        self.detached = true; // prevent Drop from double-destroying
        unsafe {
            #[cfg(not(zero_heap))]
            bindings::ove_thread_destroy(handle);
            #[cfg(zero_heap)]
            bindings::ove_thread_deinit(handle);
        }
        // SAFETY: T is required by the type system to be () for now —
        // we transmute a unit to T because PhantomData<fn() -> T>
        // doesn't constrain T at runtime.  Once the substrate surfaces
        // a worker return value this becomes a real read.
        Ok(unsafe { core::mem::MaybeUninit::<T>::zeroed().assume_init() })
    }

    /// Consume the handle without joining or requesting stop.  The
    /// underlying kernel thread keeps running; its resources are leaked
    /// from the binding's perspective (the RTOS may reap them when the
    /// entry function eventually returns).
    ///
    /// Use this when the worker is fire-and-forget and you don't want
    /// the join wait that [`Drop`] would otherwise do.  Prefer it over
    /// `core::mem::forget(handle)` because the intent is visible at the
    /// call site.
    pub fn detach(mut self) {
        self.detached = true;
        // Drop runs immediately on `self`; sees detached=true and
        // skips both request_stop and destroy.
    }

    /// Raw handle accessor for advanced use.
    #[inline]
    pub fn raw_handle(&self) -> bindings::ove_thread_t {
        self.handle
    }

    // ── Convenience delegates to Thread (non-owning operations) ──
    // These let callers do `h.suspend()` instead of `h.thread().suspend()`.

    /// Suspend the running thread.  See [`Thread::suspend`].
    #[inline]
    pub fn suspend(&self) {
        self.thread().suspend();
    }

    /// Resume the running thread.  See [`Thread::resume`].
    #[inline]
    pub fn resume(&self) {
        self.thread().resume();
    }

    /// Change the running thread's priority.  See [`Thread::set_priority`].
    #[inline]
    pub fn set_priority(&self, prio: Priority) {
        self.thread().set_priority(prio);
    }

    /// Read the running thread's state.  See [`Thread::get_state`].
    pub fn get_state(&self) -> ThreadState {
        self.thread().get_state()
    }

    /// Read the running thread's stack usage.  See [`Thread::get_stack_usage`].
    #[inline]
    pub fn get_stack_usage(&self) -> usize {
        self.thread().get_stack_usage()
    }

    /// Read the running thread's runtime stats.  See [`Thread::get_runtime_stats`].
    pub fn get_runtime_stats(&self) -> Result<ThreadStats> {
        self.thread().get_runtime_stats()
    }
}

impl<T> fmt::Debug for JoinHandle<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("JoinHandle")
            .field("handle", &format_args!("{:p}", self.handle))
            .field("detached", &self.detached)
            .finish()
    }
}

impl<T> Drop for JoinHandle<T> {
    /// Signals the cooperative-cancellation flag via
    /// [`Thread::request_stop`] then waits on the substrate's join.
    ///
    /// Workers built with [`Builder::spawn`] or
    /// [`Builder::spawn_cooperative`] (poll `StopToken::is_stopped`)
    /// exit cleanly without deadlocking.  Workers built with
    /// [`Builder::spawn_simple`] still block the join if their entry
    /// function doesn't return on its own — the stop flag is set but
    /// nothing observes it.
    ///
    /// Suppressed entirely by [`JoinHandle::detach`].
    fn drop(&mut self) {
        if !self.detached && !self.handle.is_null() {
            unsafe { bindings::ove_thread_request_stop(self.handle) };
            #[cfg(not(zero_heap))]
            unsafe {
                bindings::ove_thread_destroy(self.handle)
            };
            #[cfg(zero_heap)]
            unsafe {
                bindings::ove_thread_deinit(self.handle)
            };
        }
    }
}

// ---------------------------------------------------------------------------
// System heap statistics
// ---------------------------------------------------------------------------

/// System heap statistics.
#[derive(Debug, Clone, Copy)]
pub struct MemStats {
    /// Total heap size in bytes.
    pub total: usize,
    /// Current free heap in bytes.
    pub free: usize,
    /// Current used heap in bytes.
    pub used: usize,
    /// Peak used heap in bytes since boot.
    pub peak_used: usize,
}

/// Query system heap statistics.
///
/// # Errors
/// Returns an error if the RTOS does not provide heap statistics.
pub fn get_mem_stats() -> Result<MemStats> {
    let mut stats = bindings::ove_mem_stats {
        total: 0,
        free: 0,
        used: 0,
        peak_used: 0,
    };
    let rc = unsafe { bindings::ove_sys_get_mem_stats(&mut stats) };
    Error::from_code(rc)?;
    Ok(MemStats {
        total: stats.total,
        free: stats.free,
        used: stats.used,
        peak_used: stats.peak_used,
    })
}

// ---------------------------------------------------------------------------
// Thread enumeration
// ---------------------------------------------------------------------------

/// Snapshot of a single thread's info.
#[derive(Debug, Clone, Copy)]
pub struct ThreadInfo {
    /// Thread name (static string from RTOS).
    pub name: &'static [u8],
    /// Execution state.
    pub state: bindings::ove_thread_state_t,
    /// Priority level.
    pub priority: i32,
    /// Stack high-water mark in bytes.
    pub stack_used: usize,
}

/// List all threads in the system.
///
/// Fills the provided buffer with thread info snapshots and returns the
/// slice of entries actually written.
///
/// # Errors
/// Returns an error if the RTOS does not support thread enumeration.
pub fn thread_list(buf: &mut [ThreadInfo]) -> Result<&[ThreadInfo]> {
    const MAX_THREADS: usize = 32;
    let count = buf.len().min(MAX_THREADS);
    let mut raw: [bindings::ove_thread_info; MAX_THREADS] = unsafe { core::mem::zeroed() };
    let mut actual: usize = 0;
    let rc = unsafe { bindings::ove_thread_list(raw.as_mut_ptr(), count, &mut actual) };
    Error::from_code(rc)?;

    let actual = actual.min(count);
    for i in 0..actual {
        let name = if raw[i].name.is_null() {
            &[]
        } else {
            unsafe {
                let p = raw[i].name as *const u8;
                let mut len = 0;
                while *p.add(len) != 0 {
                    len += 1;
                }
                core::slice::from_raw_parts(p, len)
            }
        };
        buf[i] = ThreadInfo {
            name,
            state: raw[i].state,
            priority: raw[i].priority,
            stack_used: raw[i].stack_used,
        };
    }
    Ok(&buf[..actual])
}
