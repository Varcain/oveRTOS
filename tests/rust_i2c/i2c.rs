// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use core::time::Duration;
use embedded_hal::i2c::{I2c as _, Operation};
use ove::{
    Error, I2c, I2cStorage,
    i2c::{Config, Speed},
};
use std::sync::{Arc, Barrier, Mutex, Once};

unsafe extern "C" {
    fn i2c_test_reset();
    fn i2c_test_stat(index: u32) -> u32;
    fn i2c_test_fail(stage: u32);
    fn i2c_test_quarantine(enabled: i32);
    fn i2c_test_release_quarantine();
    fn i2c_test_bounds(tx: *const u8, tx_len: usize, rx: *mut u8, rx_len: usize);
    fn i2c_test_last(field: u32) -> u64;
    fn i2c_test_byte(index: u32) -> u32;
    fn i2c_test_hold_deinit();
    fn i2c_test_wait_deinit() -> i32;
    fn i2c_test_finish_deinit();
    fn i2c_test_raw_open() -> ove::ffi::ove_i2c_t;
    fn i2c_test_raw_close(handle: ove::ffi::ove_i2c_t);
    fn i2c_test_native_size() -> usize;
    fn i2c_test_native_align() -> usize;
    fn i2c_test_output() -> *const core::ffi::c_char;
}
const ALLOC: u32 = 0;
const FREE: u32 = 1;
const INIT: u32 = 2;
const DEINIT: u32 = 3;
const OPEN: u32 = 4;
const CLOSE: u32 = 5;
const HAL: u32 = 6;
const LOCK: u32 = 7;
const UNLOCK: u32 = 8;
const INVALID: u32 = 9;
const INIT_ENTRY: u32 = 10;
const DESTROY: u32 = 11;
const CONSOLE: u32 = 12;
static SERIAL: Mutex<()> = Mutex::new(());
static STORAGE: I2cStorage = I2cStorage::new();
struct Cleanup;
impl Drop for Cleanup {
    fn drop(&mut self) {
        unsafe { i2c_test_release_quarantine() };
    }
}
fn start() -> Cleanup {
    unsafe { i2c_test_reset() };
    Cleanup
}
fn stat(index: u32) -> u32 {
    unsafe { i2c_test_stat(index) }
}
fn last(field: u32) -> u64 {
    unsafe { i2c_test_last(field) }
}
fn good() {
    assert_eq!(
        stat(INVALID),
        0,
        "fixture observed an invalid lifetime/span/lock transition"
    );
}
fn native_bounds(tx: &[u8], rx: &mut [u8]) {
    unsafe { i2c_test_bounds(tx.as_ptr(), tx.len(), rx.as_mut_ptr(), rx.len()) };
}

#[test]
fn config_and_speed_grades_reach_real_frontend() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    for (speed, hz) in [
        (Speed::Standard, 100_000),
        (Speed::Fast, 400_000),
        (Speed::FastPlus, 1_000_000),
    ] {
        let bus = STORAGE.init(Config { instance: 3, speed }).unwrap();
        assert_eq!(last(7), 3);
        assert_eq!(last(8), hz);
        drop(bus);
    }
    assert_eq!(stat(OPEN), 3);
    assert_eq!(stat(INIT), 3);
    assert_eq!(stat(DEINIT), 3);
    assert_eq!(stat(CLOSE), 3);
    good();
}
#[test]
fn static_claim_blocks_live_reuse_then_reopens() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let bus = STORAGE.init(Config::default()).unwrap();
    assert!(matches!(STORAGE.init(Config::default()), Err(Error::Busy)));
    assert_eq!(
        stat(INIT_ENTRY),
        1,
        "live backing reached native initialization twice"
    );
    let address = bus.raw();
    let moved = bus;
    assert_eq!(moved.raw(), address);
    drop(moved);
    let again = STORAGE.init(Config::default()).unwrap();
    drop(again);
    assert_eq!(stat(INIT_ENTRY), 2);
    assert_eq!(stat(CLOSE), 2);
    assert_eq!(stat(DEINIT), 2);
    good();
}
#[test]
fn failed_static_initialization_releases_claim_after_cleanup() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    for (failure, error, mutex_cleanup) in [(2, Error::NoMemory, 0), (3, Error::Io, 1)] {
        unsafe { i2c_test_fail(failure) };
        let before = stat(DEINIT);
        assert!(matches!(STORAGE.init(Config::default()),Err(e) if e==error));
        assert_eq!(stat(DEINIT) - before, mutex_cleanup);
        assert_eq!(stat(CLOSE), if failure == 2 { 0 } else { 1 });
        unsafe { i2c_test_fail(0) };
        let bus = STORAGE.init(Config::default()).unwrap();
        drop(bus);
    }
    assert_eq!(stat(CLOSE), 2);
    good();
}
#[test]
fn forgotten_static_owner_keeps_one_native_claim() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    static FORGOTTEN: I2cStorage = I2cStorage::new();
    std::mem::forget(FORGOTTEN.init(Config::default()).unwrap());
    assert!(matches!(
        FORGOTTEN.init(Config::default()),
        Err(Error::Busy)
    ));
    assert_eq!(
        stat(INIT_ENTRY),
        1,
        "forgotten backing reached native initialization again"
    );
    assert_eq!(stat(CLOSE), 0);
    good();
}
#[test]
fn simultaneous_claims_initialize_only_one_owner() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    static SHARED: I2cStorage = I2cStorage::new();
    let enter = Arc::new(Barrier::new(16));
    let leave = Arc::new(Barrier::new(16));
    let winners = std::sync::atomic::AtomicU32::new(0);
    std::thread::scope(|scope| {
        for _ in 0..16 {
            let enter = enter.clone();
            let leave = leave.clone();
            let winners = &winners;
            scope.spawn(move || {
                enter.wait();
                let result = SHARED.init(Config::default());
                if result.is_ok() {
                    winners.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                }
                leave.wait();
                if result.is_err() {
                    assert!(matches!(result, Err(Error::Busy)));
                }
                drop(result);
            });
        }
    });
    assert_eq!(winners.load(std::sync::atomic::Ordering::SeqCst), 1);
    assert_eq!(
        stat(INIT_ENTRY),
        1,
        "losing claims entered native initialization"
    );
    assert_eq!(stat(CLOSE), 1);
    good();
}
#[test]
fn native_cleanup_returns_before_static_reuse() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    static SHARED: I2cStorage = I2cStorage::new();
    unsafe { i2c_test_hold_deinit() };
    let worker = std::thread::spawn(|| {
        let bus = SHARED.init(Config::default()).unwrap();
        drop(bus);
    });
    let entered = unsafe { i2c_test_wait_deinit() };
    let second = if entered == 1 {
        Some(SHARED.init(Config::default()))
    } else {
        None
    };
    unsafe { i2c_test_finish_deinit() };
    worker.join().unwrap();
    assert_eq!(entered, 1, "native cleanup gate was not reached");
    assert!(matches!(second, Some(Err(Error::Busy))));
    assert_eq!(
        stat(INIT_ENTRY),
        1,
        "claim released before native cleanup returned"
    );
    drop(SHARED.init(Config::default()).unwrap());
    good();
}
#[test]
#[cfg(not(zero_heap))]
fn heap_drop_reclaims_real_native_allocation() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let bus = I2c::new(Config::default()).unwrap();
    let moved = bus;
    drop(moved);
    assert_eq!(stat(ALLOC), 1);
    assert_eq!(stat(FREE), 1);
    assert_eq!(stat(DESTROY), 1);
    assert_eq!(stat(CLOSE), 1);
    assert_eq!(stat(DEINIT), 1);
    good();
}
#[test]
#[cfg(not(zero_heap))]
fn heap_lifecycle_and_rejected_out_handles_are_balanced() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    // Quarantine makes wrong/double cleanup mutations observable without
    // invalid access; ordinary real-free behavior has a separate case.
    unsafe { i2c_test_quarantine(1) };
    drop(I2c::new(Config::default()).unwrap());
    assert_eq!(stat(DESTROY), 1);
    assert_eq!(stat(FREE), 1);
    good();
    for (failure, error) in [(1, Error::NoMemory), (2, Error::NoMemory), (3, Error::Io)] {
        unsafe { i2c_test_fail(failure) };
        assert!(
            matches!(I2c::new(Config::default()),Err(e) if e==error),
            "failed native create became an owner"
        );
    }
    assert_eq!(stat(ALLOC), 4);
    assert_eq!(stat(FREE), 3);
    assert_eq!(
        stat(DESTROY),
        1,
        "an error out handle was destroyed as an owner"
    );
    good();
}
#[test]
fn unsafe_borrowed_wrapper_preserves_external_ownership() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let handle = unsafe { i2c_test_raw_open() };
    assert!(!handle.is_null());
    let mut borrowed = unsafe { I2c::from_handle(handle) };
    borrowed.probe(0x42, Duration::ZERO).unwrap();
    drop(borrowed);
    let before = stat(CLOSE);
    let invalid = stat(INVALID);
    unsafe { i2c_test_raw_close(handle) };
    assert_eq!(before, 0, "borrowed Drop closed an external native owner");
    assert_eq!(invalid, 0);
    good();
}
#[test]
fn inherent_operations_preserve_spans_prefixes_and_lengths() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let mut bus = STORAGE.init(Config::default()).unwrap();
    let tx = [1, 2, 3];
    let mut rx = [0u8; 9];
    native_bounds(&tx, &mut rx[1..8]);
    bus.write_read(0x42, &tx, &mut rx[1..8], Duration::from_nanos(123))
        .unwrap();
    assert_eq!((last(0), last(2), last(3), last(4)), (3, 3, 7, 123));
    assert_eq!(rx[0], 0);
    assert_eq!(rx[8], 0);
    assert_eq!(&rx[1..8], &[0x5a; 7]);
    for (i, value) in tx.iter().enumerate() {
        assert_eq!(unsafe { i2c_test_byte(i as u32) }, *value as u32);
    }
    bus.reg_write(0x42, 0x17, &tx, Duration::ZERO).unwrap();
    assert_eq!((last(0), last(2), last(3)), (1, 4, 0));
    assert_eq!(unsafe { i2c_test_byte(0) }, 0x17);
    bus.reg_read(0x42, 0x18, &mut rx[1..6], Duration::ZERO)
        .unwrap();
    assert_eq!((last(0), last(2), last(3)), (3, 1, 5));
    assert_eq!(unsafe { i2c_test_byte(0) }, 0x18);
    bus.write(0, &tx, Duration::ZERO).unwrap();
    assert_eq!(last(1), 0);
    bus.read(127, &mut rx, Duration::ZERO).unwrap();
    assert_eq!(last(1), 127);
    drop(bus);
    assert_eq!(stat(LOCK), stat(UNLOCK));
    good();
}
#[test]
fn zero_write_and_probe_dispatch_but_inherent_empty_prefix_reads() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let mut bus = STORAGE.init(Config::default()).unwrap();
    bus.write(0x42, &[], Duration::ZERO).unwrap();
    assert_eq!((last(0), last(2)), (1, 0));
    bus.probe(0x42, Duration::ZERO).unwrap();
    assert_eq!((last(0), last(2), last(5)), (1, 0, 0));
    bus.write_read(0x42, &[], &mut [0; 3], Duration::ZERO)
        .unwrap();
    assert_eq!((last(0), last(2), last(3)), (2, 0, 3));
    assert_eq!(stat(HAL), 3);
    drop(bus);
    good();
}
#[test]
fn unsupported_trait_shapes_reject_before_any_prefix_effect() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let mut bus = STORAGE.init(Config::default()).unwrap();
    let tx = [1, 2, 3];
    let mut a = [0; 4];
    let mut b = [0; 5];
    assert_eq!(
        bus.transaction(0x42, &mut [Operation::Write(&tx), Operation::Write(&tx)]),
        Err(Error::NotSupported)
    );
    assert_eq!(
        bus.transaction(
            0x42,
            &mut [Operation::Read(&mut a), Operation::Read(&mut b)]
        ),
        Err(Error::NotSupported)
    );
    assert_eq!(
        bus.transaction(0x42, &mut [Operation::Read(&mut a), Operation::Write(&tx)]),
        Err(Error::NotSupported)
    );
    assert_eq!(
        bus.transaction(
            0x42,
            &mut [
                Operation::Write(&tx),
                Operation::Read(&mut a),
                Operation::Write(&tx)
            ]
        ),
        Err(Error::NotSupported)
    );
    assert_eq!(
        bus.transaction(0x42, &mut [Operation::Write(&[]), Operation::Read(&mut a)]),
        Err(Error::NotSupported)
    );
    assert_eq!(
        embedded_hal::i2c::I2c::write_read(&mut bus, 0x42, &[], &mut a),
        Err(Error::NotSupported)
    );
    assert_eq!(
        bus.transaction(
            0x42,
            &mut [
                Operation::Write(&tx),
                Operation::Write(&tx),
                Operation::Read(&mut [])
            ]
        ),
        Err(Error::NotSupported)
    );
    assert_eq!(stat(HAL), 0, "unsupported vector performed a native prefix");
    assert_eq!(stat(LOCK), 0);
    drop(bus);
    good();
}
#[test]
fn accepted_trait_shapes_have_one_native_transaction() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let mut bus = STORAGE.init(Config::default()).unwrap();
    let tx = [3, 4, 5];
    let mut rx = [0; 7];
    bus.transaction(0x42, &mut []).unwrap();
    assert_eq!(stat(HAL), 0);
    bus.transaction(0x42, &mut [Operation::Write(&[])]).unwrap();
    assert_eq!((last(0), last(2)), (1, 0));
    embedded_hal::i2c::I2c::write(&mut bus, 0x42, &tx).unwrap();
    assert_eq!(last(0), 1);
    embedded_hal::i2c::I2c::read(&mut bus, 0x42, &mut rx).unwrap();
    assert_eq!(last(0), 2);
    native_bounds(&tx, &mut rx);
    bus.transaction(0x42, &mut [Operation::Write(&tx), Operation::Read(&mut rx)])
        .unwrap();
    assert_eq!((last(0), last(2), last(3), last(4)), (3, 3, 7, u64::MAX));
    assert_eq!(stat(HAL), 4);
    drop(bus);
    good();
}
#[test]
fn invalid_inputs_and_native_errors_preserve_dispatch_contracts() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let mut bus = STORAGE.init(Config::default()).unwrap();
    assert_eq!(bus.probe(128, Duration::ZERO), Err(Error::InvalidParam));
    assert_eq!(
        bus.write(0x180, &[1], Duration::ZERO),
        Err(Error::InvalidParam)
    );
    assert_eq!(
        bus.reg_write(0x42, 0, &[0; 33], Duration::ZERO),
        Err(Error::InvalidParam)
    );
    assert_eq!(bus.transaction(128, &mut []), Err(Error::InvalidParam));
    assert_eq!(
        embedded_hal::i2c::I2c::read(&mut bus, 0x42, &mut []),
        Err(Error::InvalidParam)
    );
    assert_eq!(
        embedded_hal::i2c::I2c::write_read(&mut bus, 0x42, &[1], &mut []),
        Err(Error::InvalidParam)
    );
    assert_eq!(stat(HAL), 0);
    assert_eq!(stat(LOCK), 0);
    unsafe { i2c_test_fail(4) };
    assert_eq!(
        bus.read(0x42, &mut [0; 3], Duration::ZERO),
        Err(Error::BusNack)
    );
    assert_eq!((stat(LOCK), stat(UNLOCK)), (1, 1));
    drop(bus);
    good();
}
#[test]
fn duration_conversion_preserves_nanoseconds_and_saturates() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    let mut bus = STORAGE.init(Config::default()).unwrap();
    for (duration, expected) in [
        (Duration::ZERO, 0),
        (Duration::from_nanos(1), 1),
        (Duration::from_micros(999), 999000),
        (Duration::from_nanos(u64::MAX), u64::MAX),
        (Duration::MAX, u64::MAX),
    ] {
        bus.probe(0x42, duration).unwrap();
        assert_eq!(last(4), expected, "duration changed at the native boundary");
    }
    drop(bus);
    good();
}
#[test]
fn root_and_facade_logging_follow_optional_console() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    static LOGGER: Once = Once::new();
    LOGGER.call_once(|| {
        ove::log::init().unwrap();
        log::set_max_level(log::LevelFilter::Info);
    });
    ove::log(b"raw");
    log::info!("message");
    let output = unsafe { std::ffi::CStr::from_ptr(i2c_test_output()) }.to_bytes();
    #[cfg(has_console)]
    {
        assert_eq!(stat(CONSOLE), 2);
        assert_eq!(output, b"raw[I] message\n");
    }
    #[cfg(not(has_console))]
    {
        assert_eq!(stat(CONSOLE), 0);
        assert!(output.is_empty());
    }
    good();
}
#[test]
fn generated_storage_matches_real_native_layout() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    assert_eq!(
        core::mem::size_of::<ove::ffi::ove_i2c_storage_t>(),
        unsafe { i2c_test_native_size() }
    );
    assert_eq!(
        core::mem::align_of::<ove::ffi::ove_i2c_storage_t>(),
        unsafe { i2c_test_native_align() }
    );
    assert!(core::mem::size_of::<I2cStorage>() >= unsafe { i2c_test_native_size() } + 1);
    good();
}

#[test]
fn relative_sync_timeouts_work_without_the_optional_clock() {
    let _serial = SERIAL.lock().unwrap();
    let _cleanup = start();
    static SYNC: ove::MutexStorage = ove::MutexStorage::new();
    let mutex = ove::Mutex::create(&SYNC, 7u32).unwrap();
    mutex
        .try_with_lock_for(Duration::ZERO, |value| {
            assert_eq!(*value, 7);
            std::thread::scope(|scope| {
                scope
                    .spawn(|| {
                        assert!(matches!(
                            mutex.try_with_lock_for(Duration::ZERO, |_| Ok(())),
                            Err(Error::Timeout)
                        ));
                    })
                    .join()
                    .unwrap();
            });
            Ok(())
        })
        .unwrap();
    mutex
        .try_with_lock_for(Duration::from_nanos(1), |value| {
            assert_eq!(*value, 7);
            Ok(())
        })
        .unwrap();
    drop(mutex);
    good();
}
