// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS-native embassy executor.
//!
//! Wraps `embassy_executor::raw::Executor` with a run loop that blocks
//! on `ove_event_wait` between polls — yields cleanly to the
//! FreeRTOS / Zephyr / NuttX scheduler on embedded targets instead of
//! spinning on WFE or parking a host thread.
//!
//! Replaces the upstream `__pender` symbol with our own that signals
//! an `ove_event` using either `ove_event_signal` or
//! `ove_event_signal_from_isr` based on `ove_is_in_isr()`. To use this
//! executor, depend on `embassy-executor` without selecting any
//! `arch-*` / `executor-thread` feature — that keeps the upstream
//! `__pender` from being linked.
//!
//! ## Single-executor design
//!
//! [`Executor::take`] returns a `&'static mut Self` and can be called
//! exactly once per process. Multi-executor priority levels are Phase 3
//! work (per-thread-id pender dispatch).

#[cfg(zero_heap)]
use ::core::cell::UnsafeCell;
#[cfg(zero_heap)]
use ::core::mem::MaybeUninit;
use ::core::sync::atomic::{AtomicBool, Ordering};

use embassy_executor::{Spawner, raw};

use crate::bindings;
use crate::error::{Error, Result};
use crate::init_cell::InitCell;
use crate::sync::Event;

/// One-time-init store for the global pender event. The executor's
/// run loop blocks on `evt.wait()` between polls; the `__pender`
/// trampoline (called by Embassy at every waker.wake) signals it.
static PENDER_EVENT: InitCell<Event> = InitCell::new();

/// Executor singleton guard.
static EXECUTOR_TAKEN: AtomicBool = AtomicBool::new(false);

/// oveRTOS-native embassy executor.
pub struct Executor {
    inner: raw::Executor,
}

// SAFETY: the executor is moved across thread boundaries once at
// `take()` time (the macro spawns it on a dedicated ove::Thread) and
// thereafter mutated only on that thread. The `&'static mut` returned
// by `take()` is the only handle to it.
unsafe impl Send for Executor {}
unsafe impl Sync for Executor {}

impl Executor {
    /// Construct the global executor and return an exclusive reference.
    /// May only be called once per process; subsequent calls return
    /// [`Error::Inval`].
    ///
    /// In heap mode the executor lives in a leaked [`Box`]; in
    /// zero-heap mode it lives in a function-scope `static mut`
    /// [`MaybeUninit`] slot. Either way the returned reference has
    /// `'static` lifetime.
    pub fn take() -> Result<&'static mut Self> {
        if EXECUTOR_TAKEN.swap(true, Ordering::AcqRel) {
            return Err(Error::Inval);
        }

        #[cfg(not(zero_heap))]
        {
            extern crate alloc;
            let boxed = alloc::boxed::Box::new(Self {
                // The pender context is unused — our __pender uses the
                // global PENDER_EVENT instead.
                inner: raw::Executor::new(::core::ptr::null_mut()),
            });
            Ok(alloc::boxed::Box::leak(boxed))
        }
        #[cfg(zero_heap)]
        {
            // Function-scope static — initialised exactly once because
            // EXECUTOR_TAKEN swaps to true above. Wrapped in an
            // UnsafeCell so the static itself is Sync.
            static mut EXEC_SLOT: MaybeUninit<UnsafeCell<Executor>> = MaybeUninit::uninit();
            // SAFETY: EXEC_SLOT is touched exactly once across the
            // process; subsequent take() calls are gated by
            // EXECUTOR_TAKEN above.
            unsafe {
                let slot = &raw mut EXEC_SLOT;
                (*slot).write(UnsafeCell::new(Self {
                    inner: raw::Executor::new(::core::ptr::null_mut()),
                }));
                let cell: &UnsafeCell<Executor> = (*slot).assume_init_ref();
                Ok(&mut *cell.get())
            }
        }
    }

    /// Run the executor forever. Initialises the pender event, calls
    /// `init(spawner)` once to spawn the initial task set, then loops:
    /// `poll()` to advance ready tasks, then `ove_event_wait` until
    /// the next waker fires.
    pub fn run(&'static mut self, init: impl FnOnce(Spawner)) -> ! {
        // Lazy-init the pender event on first run.  Idempotent: if a
        // previous Executor::take + run cycle happened we keep the
        // existing event.  In practice take() prevents that — kept
        // defensive for future multi-executor support.
        if PENDER_EVENT.try_get().is_none() {
            let evt = make_pender_event().expect("create pender event");
            PENDER_EVENT.init(evt);
        }

        let spawner: Spawner = self.inner.spawner();
        init(spawner);

        loop {
            // SAFETY: we hold &'static mut self, so no concurrent
            // poll() exists; embassy's wake path is interior-mutability
            // safe.
            unsafe {
                self.inner.poll();
            }
            // Block on ove_event_wait — yields the CPU to the RTOS
            // scheduler. SysTick or any other interrupt that signals
            // a waker will fire __pender, which signals this event.
            let _ = PENDER_EVENT.get().wait();
        }
    }
}

#[cfg(not(zero_heap))]
fn make_pender_event() -> Result<Event> {
    Event::new()
}

#[cfg(zero_heap)]
fn make_pender_event() -> Result<Event> {
    // Function-scope static storage for the binary event.
    static mut STORAGE: bindings::ove_event_storage_t = unsafe { ::core::mem::zeroed() };
    // SAFETY: STORAGE is only touched here, and take()'s singleton
    // guarantees this runs once.
    unsafe { Event::from_static(::core::ptr::addr_of_mut!(STORAGE)) }
}

/// Embassy looks up this exact symbol via `extern "Rust"` linkage when
/// the user crate pulls in `embassy-executor` (without an `arch-*`
/// feature). Each `waker.wake()` lands here.
///
/// Dispatches between `ove_event_signal` (thread context) and
/// `ove_event_signal_from_isr` (ISR context) based on `ove_is_in_isr()`.
/// `_context` is the value passed to `raw::Executor::new` — we always
/// use `null_mut()` and route via the global `PENDER_EVENT`.
#[unsafe(no_mangle)]
extern "Rust" fn __pender(_context: *mut ()) {
    // PENDER_EVENT may not be initialised yet if a stray waker fires
    // before run() — e.g. during task setup. Tolerate that.
    let Some(evt) = PENDER_EVENT.try_get() else {
        return;
    };
    if unsafe { bindings::ove_is_in_isr() } {
        evt.signal_from_isr();
    } else {
        evt.signal();
    }
}
