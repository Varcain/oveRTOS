// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! RTOS thread management for oveRTOS.
//!
//! [`Thread`] wraps an RTOS task handle with RAII lifecycle management. Threads
//! can be created from safe Rust `fn()` entry points via [`Thread::spawn`], and
//! are automatically destroyed when the [`Thread`] handle is dropped.

use core::fmt;

use crate::bindings;
use crate::error::{Error, Result};

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

/// Handle to an OS thread.
///
/// For the common case of current-thread operations (`sleep_ms`, `yield_now`),
/// use the associated functions directly — no handle needed.
pub struct Thread {
    handle: bindings::ove_thread_t,
    owned: bool,
}

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
    ///
    /// The returned handle will **not** destroy the thread on drop,
    /// since the caller does not own it.
    pub fn current() -> Self {
        let handle = unsafe { bindings::ove_thread_get_self() };
        Self {
            handle,
            owned: false,
        }
    }

    /// Create a new thread via heap allocation (only in heap mode).
    ///
    /// This is the low-level API that takes an `unsafe extern "C"` entry.
    /// Prefer [`Thread::spawn()`] for safe Rust entry functions.
    ///
    /// # Errors
    /// Returns [`Error::NoMemory`] if heap allocation fails, or another error
    /// if the RTOS rejects the thread descriptor.
    #[cfg(not(zero_heap))]
    pub fn create(
        name: &[u8],
        entry: unsafe extern "C" fn(*mut core::ffi::c_void),
        priority: Priority,
        stack_size: usize,
    ) -> Result<Self> {
        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_create(
                &mut handle,
                name.as_ptr() as *const _,
                Some(entry),
                core::ptr::null_mut(),
                priority as bindings::ove_prio_t,
                stack_size,
            )
        };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            owned: true,
        })
    }

    /// Spawn a thread with a safe Rust entry function.
    ///
    /// Uses a trampoline to convert `fn()` into the `unsafe extern "C"` entry
    /// expected by the C API. No raw pointers or `unsafe` needed in user code.
    ///
    /// # Errors
    /// Returns [`Error::NoMemory`] if heap allocation fails, or another error
    /// if the RTOS rejects the thread descriptor.
    #[cfg(not(zero_heap))]
    pub fn spawn(name: &[u8], entry: fn(), priority: Priority, stack_size: usize) -> Result<Self> {
        unsafe extern "C" fn trampoline(arg: *mut core::ffi::c_void) {
            // SAFETY: `arg` was set by the caller from a `fn()` pointer.
            // On supported targets fn() is pointer-sized and C-ABI-compatible,
            // so round-tripping through `*mut c_void` is well-defined.
            let entry: fn() = unsafe { core::mem::transmute(arg) };
            entry();
        }

        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_create(
                &mut handle,
                name.as_ptr() as *const _,
                Some(trampoline),
                entry as *mut core::ffi::c_void,
                priority as bindings::ove_prio_t,
                stack_size,
            )
        };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            owned: true,
        })
    }

    /// Spawn a thread with a `FnOnce` closure that may capture state
    /// (such as `Arc<T>` clones).
    ///
    /// The closure is boxed onto the heap and dropped on the new
    /// thread once it has executed.  Requires the `alloc` feature and
    /// a registered `#[global_allocator]`; for bare-metal targets pull
    /// in `ove-allocator` (or supply your own).
    ///
    /// ```ignore
    /// let queue = Arc::new(Queue::<u32, 8>::new()?);
    /// let q = Arc::clone(&queue);
    /// let _t = Thread::spawn_with(b"prod\0", Priority::Normal, 4096, move || {
    ///     q.send(&42u32, 1000).ok();
    /// })?;
    /// ```
    ///
    /// # Errors
    /// Returns [`Error::NoMemory`] if heap allocation fails, or another error
    /// if the RTOS rejects the thread descriptor.
    #[cfg(all(not(zero_heap), feature = "alloc"))]
    pub fn spawn_with<F>(
        name: &[u8],
        priority: Priority,
        stack_size: usize,
        f: F,
    ) -> Result<Self>
    where
        F: FnOnce() + Send + 'static,
    {
        // The captured closure cannot cross the C ABI directly, so we
        // double-box it: the inner `Box<dyn FnOnce>` is the type-erased
        // closure; the outer raw pointer is what we pass through the
        // `*mut c_void` arg slot.
        type ClosureBox = alloc::boxed::Box<dyn FnOnce() + Send + 'static>;

        unsafe extern "C" fn trampoline(arg: *mut core::ffi::c_void) {
            // SAFETY: `arg` is the raw pointer minted by `Box::into_raw`
            // in `spawn_with`.  We re-take ownership here and execute
            // the closure exactly once before the Box drops.
            let boxed: alloc::boxed::Box<ClosureBox> =
                unsafe { alloc::boxed::Box::from_raw(arg as *mut ClosureBox) };
            (*boxed)();
        }

        let boxed: ClosureBox = alloc::boxed::Box::new(f);
        let outer = alloc::boxed::Box::new(boxed);
        let raw = alloc::boxed::Box::into_raw(outer);

        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_create(
                &mut handle,
                name.as_ptr() as *const _,
                Some(trampoline),
                raw as *mut core::ffi::c_void,
                priority as bindings::ove_prio_t,
                stack_size,
            )
        };
        if let Err(e) = Error::from_code(rc) {
            // Reclaim the closure so it doesn't leak on spawn failure.
            // SAFETY: trampoline never ran, so we still own `raw`.
            let _ = unsafe { alloc::boxed::Box::from_raw(raw) };
            return Err(e);
        }
        Ok(Self {
            handle,
            owned: true,
        })
    }

    /// Create from caller-provided static storage and stack.
    ///
    /// # Safety
    /// - `storage` must outlive the `Thread` and not be shared.
    /// - `stack` must point to at least `stack_size` bytes and outlive the `Thread`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_thread_storage_t,
        name: &[u8],
        entry: unsafe extern "C" fn(*mut core::ffi::c_void),
        arg: *mut core::ffi::c_void,
        priority: Priority,
        stack_size: usize,
        stack: *mut core::ffi::c_void,
    ) -> Result<Self> {
        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_init(
                &mut handle,
                storage,
                name.as_ptr() as *const _,
                Some(entry),
                arg,
                priority as bindings::ove_prio_t,
                stack_size,
                stack,
            )
        };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            owned: true,
        })
    }

    /// Spawn a thread with a safe Rust entry using static storage.
    ///
    /// Zero-heap variant of [`Thread::spawn()`] — takes caller-provided
    /// storage and stack but still uses the safe trampoline pattern.
    ///
    /// # Safety
    /// - `storage` must outlive the `Thread` and not be shared.
    /// - `stack` must point to at least `stack_size` bytes and outlive the `Thread`.
    #[cfg(zero_heap)]
    pub unsafe fn spawn_static(
        storage: *mut bindings::ove_thread_storage_t,
        stack: *mut core::ffi::c_void,
        name: &[u8],
        entry: fn(),
        priority: Priority,
        stack_size: usize,
    ) -> Result<Self> {
        unsafe extern "C" fn trampoline(arg: *mut core::ffi::c_void) {
            // SAFETY: `arg` was set by the caller from a `fn()` pointer.
            // On supported targets fn() is pointer-sized and C-ABI-compatible,
            // so round-tripping through `*mut c_void` is well-defined.
            let entry: fn() = unsafe { core::mem::transmute(arg) };
            entry();
        }

        let mut handle: bindings::ove_thread_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_thread_init(
                &mut handle,
                storage,
                name.as_ptr() as *const _,
                Some(trampoline),
                entry as *mut core::ffi::c_void,
                priority as bindings::ove_prio_t,
                stack_size,
                stack,
            )
        };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            owned: true,
        })
    }

    /// Suspend this thread.
    pub fn suspend(&self) {
        unsafe { bindings::ove_thread_suspend(self.handle) }
    }

    /// Resume this thread.
    pub fn resume(&self) {
        unsafe { bindings::ove_thread_resume(self.handle) }
    }

    /// Set this thread's priority.
    pub fn set_priority(&self, prio: Priority) {
        unsafe { bindings::ove_thread_set_priority(self.handle, prio as bindings::ove_prio_t) }
    }

    /// Get current stack usage in bytes.
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
}

impl fmt::Debug for Thread {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Thread")
            .field("handle", &format_args!("{:p}", self.handle))
            .field("owned", &self.owned)
            .finish()
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
    /// High-water-mark usage in bytes.
    pub peak_used: usize,
}

/// Query system heap statistics.
///
/// # Errors
/// Returns an error if the RTOS does not support heap statistics.
pub fn get_mem_stats() -> Result<MemStats> {
    let mut raw: bindings::ove_mem_stats = unsafe { core::mem::zeroed() };
    let rc = unsafe { bindings::ove_sys_get_mem_stats(&mut raw) };
    Error::from_code(rc)?;
    Ok(MemStats {
        total: raw.total,
        free: raw.free,
        used: raw.used,
        peak_used: raw.peak_used,
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

impl Drop for Thread {
    fn drop(&mut self) {
        if self.owned && !self.handle.is_null() {
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

// SAFETY: RTOS thread handles are shareable across threads once created.
// Access to the handle is synchronized by the RTOS itself.
unsafe impl Send for Thread {}
unsafe impl Sync for Thread {}
