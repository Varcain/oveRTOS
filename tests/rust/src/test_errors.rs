// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// Error-path coverage. The rest of the suite exercises happy paths
// (`.unwrap()` on every Result) which makes passing the tests weaker
// evidence than it should be — a regression that turns a legitimate
// error into a panic would still read as "pass" in the unwrap sites.
// These tests pin down the specific `Error::*` variant returned by
// each common failure mode so the binding surface can't silently
// drop error mapping.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Error, EventGroup, Mutex, Queue, Semaphore, WaitFlags};

fn test_mutex_try_lock_contended_returns_timeout() {
    let mtx = Mutex::new(()).unwrap();
    let _first = mtx.try_lock().unwrap();
    let rc = mtx.try_lock();
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc.as_ref().map(|_| ()));
}

fn test_mutex_guard_contention_returns_timeout() {
    let mtx = Mutex::new(()).unwrap();
    let _first = mtx.try_lock().unwrap();
    let rc = mtx.try_lock();
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc.as_ref().map(|_| ()));
}

fn test_semaphore_take_empty_returns_timeout() {
    // Semaphore::new(initial, max) — start empty so try_acquire times out.
    let sem = Semaphore::new(0, 1).unwrap();
    let rc = sem.try_acquire();
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc);
}

fn test_queue_receive_empty_returns_queue_empty() {
    /* timeout=0 + empty queue is "would-have-blocked", not "timed out".
     * See substrate P0-2 (OVE_ERR_QUEUE_EMPTY). */
    let q: Queue<u32, 4> = Queue::new().unwrap();
    let rc = q.try_recv();
    assert!(matches!(rc, Err(Error::QueueEmpty)), "got {:?}", rc);
}

fn test_queue_send_full_returns_queue_full() {
    /* timeout=0 + full queue is "would-have-blocked", not "timed out".
     * See substrate P0-1 (OVE_ERR_QUEUE_FULL). */
    let q: Queue<u32, 1> = Queue::new().unwrap();
    q.try_send(&42).unwrap();
    let rc = q.try_send(&43);
    assert!(matches!(rc, Err(Error::QueueFull)), "got {:?}", rc);
    assert_eq!(q.try_recv().unwrap(), 42);
}

fn test_eventgroup_wait_bits_timeout() {
    let eg = EventGroup::new().unwrap();
    // WaitFlags::NONE → "wake on any bit set" (WAIT_ALL absent).
    let rc = eg.try_wait_bits(0x1, WaitFlags::NONE);
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc);
}

/* ── from_code / to_code round-trip ─────────────────────────────────── */

const VARIANTS: &[(i32, Error)] = &[
    (-1, Error::NotRegistered),
    (-2, Error::InvalidParam),
    (-3, Error::NoMemory),
    (-4, Error::Timeout),
    (-5, Error::NotSupported),
    (-6, Error::QueueFull),
    (-7, Error::MlFailed),
    (-8, Error::NetRefused),
    (-9, Error::NetUnreachable),
    (-10, Error::NetAddrInUse),
    (-11, Error::NetReset),
    (-12, Error::NetDnsFail),
    (-13, Error::NetClosed),
    (-14, Error::BusNack),
    (-15, Error::BusBusy),
    (-16, Error::BusError),
    (-17, Error::QueueEmpty),
    (-18, Error::WouldBlock),
    (-19, Error::Eof),
    (-20, Error::Inval),
    (-21, Error::NotFound),
    (-22, Error::NetAddrNotAvailable),
    (-23, Error::AlreadyExists),
    (-24, Error::NoSpace),
    (-25, Error::NotDir),
    (-26, Error::IsDir),
    (-27, Error::NotEmpty),
    (-28, Error::ReadOnly),
    (-29, Error::Io),
    (-30, Error::Busy),
    (-31, Error::NameTooLong),
    (-32, Error::BadHandle),
    (-33, Error::Permission),
    (-34, Error::CrossDevice),
];

fn test_from_code_ok() {
    assert!(matches!(Error::from_code(0), Ok(())));
}

fn test_from_code_all_variants() {
    for (code, expected) in VARIANTS {
        let err = Error::from_code(*code).expect_err("negative code must map to Err");
        assert_eq!(err, *expected, "code {code} did not map to {expected:?}");
    }
}

fn test_from_code_unknown() {
    // Any code outside the known table becomes `Error::Unknown(code)`.
    let err = Error::from_code(-999).expect_err("unknown negative code must be Err");
    assert_eq!(err, Error::Unknown(-999));
    // Unknown preserves positive codes too, but positive codes aren't errors
    // in the mapping table — the match's catch-all still wraps them.
    let err = Error::from_code(42).expect_err("any non-zero code must be Err");
    assert_eq!(err, Error::Unknown(42));
}

fn test_to_code_all_variants() {
    for (code, variant) in VARIANTS {
        assert_eq!(variant.to_code(), *code);
    }
    assert_eq!(Error::Unknown(-123).to_code(), -123);
}

fn test_round_trip_all_variants() {
    for (code, variant) in VARIANTS {
        let err = Error::from_code(*code).unwrap_err();
        assert_eq!(err.to_code(), *code);
        assert_eq!(err, *variant);
    }
}

/* ── Classifier helpers ─────────────────────────────────────────────── */

fn test_is_net_error_positive() {
    for v in [
        Error::NetRefused,
        Error::NetUnreachable,
        Error::NetAddrInUse,
        Error::NetAddrNotAvailable,
        Error::NetReset,
        Error::NetDnsFail,
        Error::NetClosed,
    ] {
        assert!(v.is_net_error(), "{v:?} should be net error");
        assert!(!v.is_bus_error(), "{v:?} should not be bus error");
    }
}

fn test_is_bus_error_positive() {
    for v in [Error::BusNack, Error::BusBusy, Error::BusError] {
        assert!(v.is_bus_error(), "{v:?} should be bus error");
        assert!(!v.is_net_error(), "{v:?} should not be net error");
    }
}

fn test_is_net_is_bus_negative() {
    for v in [
        Error::NotRegistered,
        Error::InvalidParam,
        Error::NoMemory,
        Error::Timeout,
        Error::NotSupported,
        Error::QueueFull,
        Error::MlFailed,
        Error::Unknown(-42),
    ] {
        assert!(!v.is_net_error(), "{v:?} should not classify as net");
        assert!(!v.is_bus_error(), "{v:?} should not classify as bus");
    }
}

/* ── Display ────────────────────────────────────────────────────────── */

fn test_display_all_variants() {
    use std::string::ToString;
    let expected = [
        (Error::NotRegistered, "not registered"),
        (Error::InvalidParam, "invalid parameter"),
        (Error::NoMemory, "out of memory"),
        (Error::Timeout, "timeout"),
        (Error::NotSupported, "not supported"),
        (Error::QueueFull, "queue full"),
        (Error::MlFailed, "ML inference failed"),
        (Error::NetRefused, "connection refused"),
        (Error::NetUnreachable, "network unreachable"),
        (Error::NetAddrInUse, "address in use"),
        (Error::NetAddrNotAvailable, "address not available"),
        (Error::NetReset, "connection reset"),
        (Error::NetDnsFail, "DNS resolution failed"),
        (Error::NetClosed, "connection closed"),
        (Error::BusNack, "bus NACK"),
        (Error::BusBusy, "bus busy"),
        (Error::BusError, "bus error"),
        (Error::QueueEmpty, "queue empty"),
        (Error::WouldBlock, "would block"),
        (Error::Eof, "end of file"),
        (Error::Inval, "invalid argument"),
        (Error::NotFound, "not found"),
        (Error::AlreadyExists, "already exists"),
        (Error::NoSpace, "no space left"),
        (Error::NotDir, "not a directory"),
        (Error::IsDir, "is a directory"),
        (Error::NotEmpty, "directory not empty"),
        (Error::ReadOnly, "read-only filesystem"),
        (Error::Io, "I/O error"),
        (Error::Busy, "resource busy"),
        (Error::NameTooLong, "name too long"),
        (Error::BadHandle, "bad handle"),
        (Error::Permission, "permission denied"),
        (Error::CrossDevice, "cross-device operation"),
    ];
    for (v, s) in expected {
        assert_eq!(v.to_string(), s, "display mismatch for {v:?}");
    }
    assert_eq!(Error::Unknown(-42).to_string(), "unknown error (-42)");
}

/* ── Derive surface ─────────────────────────────────────────────────── */

fn test_derives() {
    let a = Error::Timeout;
    let b = a;
    assert_eq!(a, b);
    assert_ne!(Error::Timeout, Error::NotRegistered);
    assert_ne!(Error::Unknown(1), Error::Unknown(2));
    // Debug is derived — exercise the path.
    let _ = format!("{:?}", Error::BusNack);
}

/* ── core::error::Error trait ───────────────────────────────────────── */

fn test_core_error_coercion() {
    // Anyhow / thiserror downstream consumers need `&dyn core::error::Error`
    // coercion to work.  This test pins that compile-time property.
    fn as_core_error(e: &Error) -> &dyn core::error::Error {
        e
    }
    let err = Error::Timeout;
    let dyn_err: &dyn core::error::Error = as_core_error(&err);
    // Sanity: `core::error::Error` requires `Display`, so the trait
    // object's `to_string` should reach our `impl Display`.
    assert_eq!(dyn_err.to_string(), "timeout");
    // Default `source()` returns None — we don't chain.
    assert!(dyn_err.source().is_none());
}

fn test_std_error_via_re_export() {
    // `std::error::Error` is `pub use core::error::Error;` since 1.81,
    // so the single `impl core::error::Error for Error` covers both
    // sides.  This test exists to catch a future stdlib reshuffle.
    fn assert_std_error<E: std::error::Error>(_: &E) {}
    assert_std_error(&Error::QueueFull);
}

pub fn run() -> (usize, usize) {
    run_suite("Errors", &[
        test_entry!(test_mutex_try_lock_contended_returns_timeout),
        test_entry!(test_mutex_guard_contention_returns_timeout),
        test_entry!(test_semaphore_take_empty_returns_timeout),
        test_entry!(test_queue_receive_empty_returns_queue_empty),
        test_entry!(test_queue_send_full_returns_queue_full),
        test_entry!(test_eventgroup_wait_bits_timeout),
        test_entry!(test_from_code_ok),
        test_entry!(test_from_code_all_variants),
        test_entry!(test_from_code_unknown),
        test_entry!(test_to_code_all_variants),
        test_entry!(test_round_trip_all_variants),
        test_entry!(test_is_net_error_positive),
        test_entry!(test_is_bus_error_positive),
        test_entry!(test_is_net_is_bus_negative),
        test_entry!(test_display_all_variants),
        test_entry!(test_derives),
        test_entry!(test_core_error_coercion),
        test_entry!(test_std_error_via_re_export),
    ])
}
