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

// SAFETY: `OveTimeDriver` is the singleton embassy time driver.  Its
// `queue` field is a `CsMutex<RefCell<Queue<_>>>` which synchronises
// access via `critical_section`; `alarm: InitMut<AlarmTimer>` is touched
// only by the scheduler thread once initialised.
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
    /// the sentinel `u64::MAX` (queue empty) we disarm; embassy_time_
    /// queue_utils uses MAX to mean "no scheduled wake".
    fn reprogram_alarm(&self, at_us: u64, now_us: u64) {
        // Ensure the alarm exists. Construct on first use.
        if self.alarm.try_get().is_none() {
            self.ensure_alarm();
        }
        let handle = self.alarm.get().handle;

        if at_us == u64::MAX {
            // Empty queue. Stop the alarm so we don't fire spuriously.
            // SAFETY: handle came from ove_timer_init_ns; stop is
            // idempotent.
            unsafe {
                let _ = bindings::ove_timer_stop(handle);
            }
            return;
        }

        // Compute the delay. Saturating sub guards against the wake
        // being already overdue (in which case we want to fire ASAP,
        // clamped to at least 1 ns by the backend).
        let delay_us = at_us.saturating_sub(now_us);
        let delay_ns: u64 = delay_us.saturating_mul(1_000).max(1);

        // SAFETY: ove_timer_set_period_ns atomically changes the
        // timer's period and rearms it with the new period — no
        // recreate of the underlying kernel timer (the previous
        // approach of init_ns + start corrupted FreeRTOS's timer list
        // because xTimerCreateStatic was called on a slot the kernel
        // still held in its daemon-task list).
        unsafe {
            let _ = bindings::ove_timer_set_period_ns(handle, delay_ns);
        }
    }

    /// Lazily construct the alarm timer.
    ///
    /// The timer is created in the stopped state with a placeholder
    /// period. `reprogram_alarm` rewrites the period and arms it via
    /// `ove_timer_set_period_ns` — the kernel-side timer object stays
    /// the same across re-arms.
    fn ensure_alarm(&self) {
        #[cfg(not(zero_heap))]
        {
            // Heap mode: ove_timer_create_ns mallocs the backend
            // storage internally and returns a handle good for the
            // program lifetime.
            let mut handle: bindings::ove_timer_t = ptr::null_mut();
            // SAFETY: heap-allocated create variant; period gets
            // overwritten by the next reprogram_alarm call.
            let rc = unsafe {
                bindings::ove_timer_create_ns(
                    &mut handle,
                    Some(alarm_fired),
                    ptr::null_mut(),
                    1_000_000, /* 1 ms placeholder, overwritten on first use */
                    1,         /* one_shot */
                )
            };
            assert!(rc == 0, "ove_timer_create_ns failed at alarm init");
            self.alarm.init(AlarmTimer { handle });
        }
        #[cfg(zero_heap)]
        {
            // Zero-heap: backend storage is a static slot in BSS,
            // initialised by ove_timer_init_ns. Matches the pattern
            // used by `ove::timer!` macro. ensure_alarm() is gated to
            // run exactly once (InitMut::try_get + init pair).
            static mut ALARM_STORAGE: bindings::ove_timer_storage_t =
                unsafe { ::core::mem::zeroed() };
            let mut handle: bindings::ove_timer_t = ptr::null_mut();
            // SAFETY: ALARM_STORAGE is touched only by the single
            // ensure_alarm() call (guarded by InitMut::try_get); the
            // raw pointer obtained via addr_of_mut! satisfies the
            // borrow-check rules for `static mut`.
            let rc = unsafe {
                bindings::ove_timer_init_ns(
                    &mut handle,
                    ::core::ptr::addr_of_mut!(ALARM_STORAGE),
                    Some(alarm_fired),
                    ptr::null_mut(),
                    1_000_000, /* 1 ms placeholder, overwritten on first use */
                    1,         /* one_shot */
                )
            };
            assert!(rc == 0, "ove_timer_init_ns failed at alarm init");
            self.alarm.init(AlarmTimer { handle });
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
extern "C" fn alarm_fired(_timer: bindings::ove_timer_t, _user_data: *mut ::core::ffi::c_void) {
    ::critical_section::with(|cs| {
        let now = DRIVER.now();
        let mut q = DRIVER.queue.borrow(cs).borrow_mut();
        let next = q.next_expiration(now);
        DRIVER.reprogram_alarm(next, now);
    });
}
