// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::{Event, EventStorage};
use std::sync::atomic::{AtomicU32, Ordering};

unsafe extern "C" {
    fn fixture_binding_start();
    fn fixture_ready_count(count: u32);
    fn fixture_prepare_signal();
    fn fixture_await_signal();
    fn fixture_binding_report(language: *const core::ffi::c_char) -> i32;
}
static STORAGE: EventStorage = EventStorage::new();

fn main() {
    // SAFETY: these calls coordinate initialized test observers only. Event
    // creation, sharing, waits and posts below use the public safe binding.
    unsafe { fixture_binding_start() };
    let completed = AtomicU32::new(0);
    {
        let event = Event::create(&STORAGE).expect("create shared event");
        std::thread::scope(|scope| {
            for _ in 0..2 {
                scope.spawn(|| {
                    event.wait().expect("consume shared occurrence");
                    completed.fetch_add(1, Ordering::Relaxed);
                });
            }
            // SAFETY: the observer waits for initialized native-entry calls.
            unsafe { fixture_ready_count(2) };
            for _ in 0..2 {
                // SAFETY: snapshot only the observer's synchronized counters.
                unsafe { fixture_prepare_signal() };
                event.signal();
                // SAFETY: bounded observer progress; old missed hints are
                // recorded before releasing the still-live test entry.
                unsafe { fixture_await_signal() };
            }
        });
        assert_eq!(completed.load(Ordering::Relaxed), 2);
    }
    // SAFETY: a static NUL-terminated language label; all workers have joined.
    let status = unsafe { fixture_binding_report(c"rust".as_ptr()) };
    std::process::exit(status);
}
