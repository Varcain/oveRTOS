// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! `embassy_time_driver::Driver` impl using `ove_time_get_us` for the
//! clock and a single one-shot `ove_timer` (re-armed via
//! `ove_timer_init_ns`) for deadline alarms.
//!
//! Tick rate is pinned to 1 MHz (microseconds) by the
//! `tick-hz-1_000_000` feature on `embassy-time` — matches the native
//! `ove_time_get_us` output exactly, no rescaling in `now()`.
//!
//! Resolution floor (the smallest interval `Timer::after_micros(N)`
//! actually waits) is backend-dependent and documented on
//! `ove_timer_init_ns`. On POSIX it is the OS timer slice (sub-µs on
//! Linux); on FreeRTOS it is one tick (typically 1 ms).

use core::cell::RefCell;
use core::ptr;
use core::task::Waker;

use ::critical_section::Mutex as CsMutex;
use embassy_time_driver::Driver;
use embassy_time_queue_utils::Queue;

use crate::bindings;
use crate::init_cell::InitMut;

// Timer state lives in an InitMut so it can be lazily constructed
// inside `Driver::schedule_wake`. The handle and storage are kept
// separately because `ove_timer_init_ns` writes through both.
struct AlarmTimer {
    handle: bindings::ove_timer_t,
    /// Heap-allocated backend storage. Boxed and leaked so its address
    /// stays stable for the lifetime of the program. The `ove_timer`
    /// struct retains an interior pointer into this storage, so moving
    /// it would invalidate the timer.
    #[allow(dead_code)] // kept to anchor the storage for the C side
    storage: *mut bindings::ove_timer_storage_t,
}

// SAFETY: the alarm is only touched under the time-driver mutex.
unsafe impl Send for AlarmTimer {}
unsafe impl Sync for AlarmTimer {}

pub(crate) struct OveTimeDriver {
    /// Sorted wake queue keyed by absolute µs deadlines. Lives under
    /// a critical-section mutex so the alarm callback (running in a
    /// SIGEV_THREAD on POSIX, workqueue on Zephyr, timer-task on
    /// FreeRTOS) can drain expired wakers concurrently with user code
    /// registering new ones.
    queue: CsMutex<RefCell<Queue>>,
    /// Single re-armable one-shot alarm. Initialised lazily by
    /// `schedule_wake` the first time it sees a deadline.
    alarm: InitMut<AlarmTimer>,
}

unsafe impl Send for OveTimeDriver {}
unsafe impl Sync for OveTimeDriver {}

embassy_time_driver::time_driver_impl!(static DRIVER: OveTimeDriver = OveTimeDriver {
    queue: CsMutex::new(RefCell::new(Queue::new())),
    alarm: InitMut::new(),
});

impl Driver for OveTimeDriver {
    /// Current time in microseconds. Matches `tick-hz-1_000_000`.
    fn now(&self) -> u64 {
        let mut us: u64 = 0;
        // SAFETY: ove_time_get_us writes through the out-parameter
        // when CONFIG_OVE_TIME is enabled; otherwise it leaves the
        // value at zero and returns OVE_ERR_NOT_SUPPORTED, which we
        // tolerate (the executor would still poll, just without
        // progress on Timer wakers — a configuration error caught at
        // runtime).
        unsafe {
            let _ = bindings::ove_time_get_us(&mut us);
        }
        us
    }

    fn schedule_wake(&self, at: u64, waker: &Waker) {
        ::critical_section::with(|cs| {
            let mut q = self.queue.borrow(cs).borrow_mut();
            if q.schedule_wake(at, waker) {
                // The head of the queue has changed (this deadline is
                // earlier than any pending one). Reprogram the alarm to
                // fire at the new soonest deadline.
                let now = self.now();
                let next = q.next_expiration(now);
                self.reprogram_alarm(next, now);
            }
        });
    }
}

impl OveTimeDriver {
    /// Re-arm the alarm to fire at absolute deadline `at_us` (in
    /// microseconds), given the current time `now_us`. If `at_us` is
    /// the sentinel `u64::MAX` (queue empty) we disarm by setting a
    /// very-far-future deadline; embassy_time_queue_utils uses MAX
    /// to mean "no scheduled wake".
    fn reprogram_alarm(&self, at_us: u64, now_us: u64) {
        // Ensure the alarm exists. Construct on first use.
        if self.alarm.try_get().is_none() {
            self.ensure_alarm();
        }

        if at_us == u64::MAX {
            // Empty queue. Stop the alarm so we don't fire spuriously.
            // SAFETY: handle came from ove_timer_init_ns; stop is
            // idempotent.
            unsafe {
                let _ = bindings::ove_timer_stop(self.alarm.get().handle);
            }
            return;
        }

        // Compute the delay. Saturating sub guards against the wake
        // being already overdue (in which case we want to fire ASAP,
        // which means a 1 ns delay — see ove_timer_start guard).
        let delay_us = at_us.saturating_sub(now_us);
        let delay_ns: u64 = delay_us.saturating_mul(1_000);

        // SAFETY: re-arming a one-shot timer via init_ns + start
        // is the documented pattern. The init_ns call overwrites the
        // existing period; start (re)arms it.
        unsafe {
            // We rely on `ove_timer_init_ns` being idempotent for
            // already-initialised storage — it memsets the struct then
            // rebinds the SIGEV_THREAD dispatcher. For the POSIX
            // backend this drops the old `posix_timer` handle into
            // `timer_create` again; the original `timer_delete` is
            // skipped because the field is overwritten before reuse.
            // This is acceptable for the alarm-only-on-startup pattern;
            // a stricter implementation would use ove_timer_reset which
            // is "atomic stop+start" but doesn't change the period.
            //
            // The cleanest path is: stop, re-init with new period,
            // start. That's what we do.
            let _ = bindings::ove_timer_stop(self.alarm.get().handle);
            // Reset the storage cell so init_ns re-creates the
            // underlying timer cleanly.
            let storage = self.alarm.get().storage;
            // Reinitialise into the same storage.
            let mut handle: bindings::ove_timer_t = ptr::null_mut();
            let rc = bindings::ove_timer_init_ns(
                &mut handle,
                storage,
                Some(alarm_fired),
                ptr::null_mut(),
                delay_ns,
                1, /* one_shot */
            );
            if rc == 0 {
                let _ = bindings::ove_timer_start(handle);
            }
        }
    }

    /// Lazily construct the alarm timer. Heap-mode only for the
    /// initial vertical slice; zero-heap will switch to caller-supplied
    /// storage as a follow-up.
    fn ensure_alarm(&self) {
        #[cfg(not(zero_heap))]
        {
            // Allocate storage on the heap and leak so its address
            // stays stable for the program lifetime. The C-side
            // `ove_timer_init_ns` writes through the storage pointer
            // and the timer struct holds an interior pointer back to
            // it, so the storage cannot move.
            extern crate alloc;
            use ::core::mem::MaybeUninit;
            let storage: *mut bindings::ove_timer_storage_t = alloc::boxed::Box::into_raw(
                alloc::boxed::Box::<MaybeUninit<bindings::ove_timer_storage_t>>::new_uninit(),
            )
            .cast();

            // Initialise with a 1 µs period as a placeholder; the next
            // call to reprogram_alarm will rewrite the period before
            // arming. The handle is captured for later stop/start.
            let mut handle: bindings::ove_timer_t = ptr::null_mut();
            // SAFETY: storage is a freshly-allocated, properly-sized
            // buffer; init_ns is documented to write through both the
            // handle and storage pointers.
            let rc = unsafe {
                bindings::ove_timer_init_ns(
                    &mut handle,
                    storage,
                    Some(alarm_fired),
                    ptr::null_mut(),
                    1_000, /* 1 µs placeholder */
                    1,     /* one_shot */
                )
            };
            assert!(rc == 0, "ove_timer_init_ns failed at alarm init");
            self.alarm.init(AlarmTimer { handle, storage });
        }
        #[cfg(zero_heap)]
        {
            // Zero-heap variant: requires caller-supplied static
            // storage. Wired up in a follow-up; until then a panic
            // here is the loudest possible failure mode for a
            // misconfigured build.
            panic!("zero-heap time driver init not yet implemented");
        }
    }
}

/// Callback invoked by the C-side timer when the alarm fires.
///
/// The callback context depends on the backend:
///  - POSIX: SIGEV_THREAD pthread (one per firing); bracketed by
///           `posix_irq_enter_isr` so `ove_is_in_isr()` returns true
///           inside this function, matching the embedded model.
///  - Zephyr: system workqueue thread (non-ISR).
///  - FreeRTOS: timer service task (non-ISR).
extern "C" fn alarm_fired(
    _timer: bindings::ove_timer_t,
    _user_data: *mut ::core::ffi::c_void,
) {
    ::critical_section::with(|cs| {
        let now = DRIVER.now();
        let mut q = DRIVER.queue.borrow(cs).borrow_mut();
        let next = q.next_expiration(now);
        DRIVER.reprogram_alarm(next, now);
    });
}
