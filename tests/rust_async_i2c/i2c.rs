// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use core::{
    future::Future,
    pin::Pin,
    task::{Context, Poll, Waker},
};
use embedded_hal_async::i2c::{I2c as _, Operation};
use ove::{
    AsyncI2c, AsyncI2cStorage, Error,
    i2c::{Config, Speed},
};
use std::sync::{Arc, Barrier, Mutex};
#[repr(C)]
#[derive(Default, Copy, Clone, Debug)]
struct Stats {
    allocations: usize,
    frees: usize,
    live: usize,
    bytes: usize,
    peak_bytes: usize,
    attempts: usize,
    submissions: usize,
    completions: usize,
    opens: usize,
    closes: usize,
    invalid: usize,
    prepares: usize,
    mutex_inits: usize,
    mutex_deinits: usize,
}
unsafe extern "C" {
    fn async_i2c_probe_reset();
    fn async_i2c_probe_stats(out: *mut Stats);
    fn async_i2c_probe_complete(result: i32);
    fn async_i2c_probe_mode(mode: i32);
    fn async_i2c_probe_limit(bytes: usize);
    #[cfg(not(zero_heap))]
    fn async_i2c_probe_fail_alloc(after: i32);
    fn async_i2c_probe_prepare_error(error: i32);
    fn async_i2c_probe_open_error(error: i32);
    fn async_i2c_probe_mutex_error(error: i32);
    fn async_i2c_probe_hold_deinit();
    fn async_i2c_probe_wait_deinit() -> i32;
    fn async_i2c_probe_release_deinit();
    fn async_i2c_probe_check(address: u16, tx: usize, rx: usize) -> i32;
    fn async_i2c_probe_tx_len() -> usize;
    fn async_i2c_probe_rx_len() -> usize;
    fn async_i2c_probe_tx_byte(index: usize) -> u32;
    fn async_i2c_probe_tx_tail() -> u32;
    fn async_i2c_probe_config(field: u32) -> u32;
    fn async_i2c_probe_pending_tx() -> usize;
    fn async_i2c_probe_pending_rx() -> usize;
    fn async_i2c_probe_watch(tx: usize, tn: usize, rx: usize, rn: usize);
    fn async_i2c_probe_release_app();
    #[cfg(not(zero_heap))]
    fn async_i2c_probe_clear_watch();
    fn async_i2c_probe_local_irq_model(enabled: i32);
    fn async_i2c_probe_irq_depth() -> u32;
}
static SERIAL: Mutex<()> = Mutex::new(());
static BUS: AsyncI2cStorage<64, 128> = AsyncI2cStorage::new();
fn reset() {
    unsafe { async_i2c_probe_reset() };
}
fn stats() -> Stats {
    let mut out = Stats::default();
    unsafe { async_i2c_probe_stats(&mut out) };
    out
}
fn clean() {
    let s = stats();
    assert_eq!((s.live, s.bytes, s.invalid), (0, 0, 0));
    assert_eq!(s.allocations, s.frees);
    assert_eq!(s.opens, s.closes);
    assert_eq!(s.mutex_inits, s.mutex_deinits);
    assert!(s.prepares >= s.submissions);
}
fn bus() -> AsyncI2c {
    #[cfg(not(zero_heap))]
    {
        AsyncI2c::new(Config::default()).unwrap()
    }
    #[cfg(zero_heap)]
    {
        BUS.init(Config::default()).unwrap()
    }
}
fn poll<F: Future + Unpin>(future: &mut F) -> Poll<F::Output> {
    let result = Pin::new(future).poll(&mut Context::from_waker(Waker::noop()));
    assert_eq!(unsafe { async_i2c_probe_irq_depth() }, 0);
    result
}
fn complete(result: ove::Result<()>) {
    unsafe { async_i2c_probe_complete(result.map_or_else(Error::to_code, |_| 0)) };
}
fn watch(tx: &[u8], rx: &mut [u8]) {
    unsafe {
        async_i2c_probe_watch(
            if tx.is_empty() {
                0
            } else {
                tx.as_ptr() as usize
            },
            tx.len(),
            if rx.is_empty() {
                0
            } else {
                rx.as_mut_ptr() as usize
            },
            rx.len(),
        )
    };
}
fn run_write(bus: &mut AsyncI2c, data: &[u8]) {
    watch(data, &mut []);
    let mut f = bus.write(0x42, data);
    assert_eq!(poll(&mut f), Poll::Pending);
    complete(Ok(()));
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
}
fn run_read(bus: &mut AsyncI2c, data: &mut [u8]) {
    watch(&[], data);
    let mut f = bus.read(0x42, data);
    assert_eq!(poll(&mut f), Poll::Pending);
    complete(Ok(()));
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
}

#[test]
fn config_and_native_static_cleanup() {
    let _s = SERIAL.lock().unwrap();
    reset();
    for (speed, hz) in [
        (Speed::Standard, 100000),
        (Speed::Fast, 400000),
        (Speed::FastPlus, 1000000),
    ] {
        let b = BUS.init(Config { instance: 3, speed }).unwrap();
        assert_eq!(unsafe { async_i2c_probe_config(0) }, 3);
        assert_eq!(unsafe { async_i2c_probe_config(1) }, hz);
        drop(b);
    }
    assert_eq!(stats().allocations, 0);
    assert_eq!(stats().opens, 3);
    clean();
}
#[test]
fn independent_staging_payload_and_exact_subslice_copyback() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let tx = [1, 2, 3];
    let mut rx = [0xc9; 9];
    watch(&tx, &mut rx[1..8]);
    let tp = tx.as_ptr() as usize;
    let rp = rx[1..8].as_mut_ptr() as usize;
    let mut f = b.write_read(0x42, &tx, &mut rx[1..8]);
    assert_eq!(poll(&mut f), Poll::Pending);
    assert_ne!(unsafe { async_i2c_probe_pending_tx() }, tp);
    assert_ne!(unsafe { async_i2c_probe_pending_rx() }, rp);
    assert_eq!(
        (unsafe { async_i2c_probe_tx_len() }, unsafe {
            async_i2c_probe_rx_len()
        }),
        (3, 7)
    );
    complete(Ok(()));
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
    drop(f);
    assert_eq!(rx[0], 0xc9);
    assert_eq!(rx[8], 0xc9);
    assert_eq!(&rx[1..8], &[0x5a; 7]);
    for (i, n) in tx.iter().enumerate() {
        assert_eq!(unsafe { async_i2c_probe_tx_byte(i) }, u32::from(*n));
    }
    assert_eq!(unsafe { async_i2c_probe_tx_tail() }, 3);
    drop(b);
    clean();
}
fn released_payload(forget: bool, free: bool) {
    let mut b = bus();
    let mut tx = vec![1, 2, 3];
    let mut rx = vec![0; 7];
    watch(&tx, &mut rx);
    let mut f = b.write_read(0x42, &tx, &mut rx);
    assert_eq!(poll(&mut f), Poll::Pending);
    if forget {
        core::mem::forget(f)
    } else {
        drop(f)
    }
    unsafe { async_i2c_probe_release_app() };
    if free {
        drop(tx);
        drop(rx);
        complete(Ok(()));
    } else {
        tx.fill(0xe1);
        rx.fill(0xcc);
        complete(Ok(()));
        assert_eq!(rx, vec![0xcc; 7]);
    }
    assert_eq!(unsafe { async_i2c_probe_tx_byte(0) }, 1);
    assert_eq!(unsafe { async_i2c_probe_tx_tail() }, 3);
    drop(b);
    clean();
}
#[test]
fn dropped_future_keeps_only_internal_payload() {
    let _s = SERIAL.lock().unwrap();
    reset();
    released_payload(false, false);
}
#[test]
fn forgotten_future_keeps_only_internal_payload() {
    let _s = SERIAL.lock().unwrap();
    reset();
    released_payload(true, false);
}
#[test]
fn dropped_future_allows_actual_application_free() {
    let _s = SERIAL.lock().unwrap();
    reset();
    released_payload(false, true);
}
#[test]
fn forgotten_future_allows_actual_application_free() {
    let _s = SERIAL.lock().unwrap();
    reset();
    released_payload(true, true);
}
#[test]
fn owner_drop_waits_for_native_completion() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let mut f = b.write(0x42, &[1, 2, 3]);
    assert_eq!(poll(&mut f), Poll::Pending);
    drop(f);
    drop(b);
    assert_eq!(stats().closes, 0);
    assert_eq!(stats().mutex_deinits, 0);
    complete(Ok(()));
    assert_eq!(stats().closes, 1);
    clean();
}
#[test]
fn movable_owner_keeps_native_backing() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let mut buffer = [0; 7];
    let mut f = b.read(0x42, &mut buffer);
    assert_eq!(poll(&mut f), Poll::Pending);
    core::mem::forget(f);
    let pointer = unsafe { async_i2c_probe_pending_rx() };
    let mut moved = b;
    assert_eq!(unsafe { async_i2c_probe_pending_rx() }, pointer);
    complete(Ok(()));
    let mut flush = moved.flush();
    assert_eq!(poll(&mut flush), Poll::Ready(Ok(())));
    drop(flush);
    drop(moved);
    clean();
}
#[test]
fn completed_forgotten_future_does_not_leak_a_count() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let mut f = b.write(0x42, &[1]);
    assert_eq!(poll(&mut f), Poll::Pending);
    core::mem::forget(f);
    complete(Ok(()));
    drop(b);
    assert_eq!(stats().closes, 1);
    clean();
}
#[test]
fn active_orphan_is_busy_and_flush_preserves_its_error() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let mut first = b.write(0x42, &[1]);
    assert_eq!(poll(&mut first), Poll::Pending);
    core::mem::forget(first);
    let before = stats();
    let mut second = b.write(0x42, &[2]);
    assert_eq!(poll(&mut second), Poll::Ready(Err(Error::BusBusy)));
    drop(second);
    assert_eq!(stats().allocations, before.allocations);
    assert_eq!(stats().submissions, 1);
    let mut f = b.flush();
    assert_eq!(poll(&mut f), Poll::Pending);
    drop(f);
    let mut f = b.flush();
    assert_eq!(poll(&mut f), Poll::Pending);
    complete(Err(Error::Io));
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::Io)));
    drop(f);
    drop(b);
    clean();
}
#[test]
fn ready_orphan_can_be_replaced_without_copyback() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let mut rx = [0xcc; 7];
    let mut f = b.read(0x42, &mut rx);
    assert_eq!(poll(&mut f), Poll::Pending);
    drop(f);
    complete(Err(Error::Io));
    run_write(&mut b, &[1, 2, 3]);
    assert_eq!(rx, [0xcc; 7]);
    drop(b);
    clean();
}
#[test]
fn error_completion_does_not_copy_internal_rx() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let tx = [1, 2, 3];
    let mut rx = [0xcc; 7];
    watch(&tx, &mut rx);
    let mut f = b.write_read(0x42, &tx, &mut rx);
    assert_eq!(poll(&mut f), Poll::Pending);
    complete(Err(Error::BusNack));
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::BusNack)));
    drop(f);
    assert_eq!(rx, [0xcc; 7]);
    drop(b);
    clean();
}
#[test]
fn immediate_completion_and_rejection_retire_once() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    unsafe { async_i2c_probe_mode(1) };
    let mut rx = [0; 7];
    let mut f = b.write_read(0x42, &[1, 2, 3], &mut rx);
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
    drop(f);
    assert_eq!(rx, [0x5a; 7]);
    unsafe { async_i2c_probe_mode(2) };
    let mut f = b.write(0x42, &[1]);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::Io)));
    drop(f);
    assert_eq!((stats().submissions, stats().completions), (2, 1));
    unsafe { async_i2c_probe_mode(0) };
    run_write(&mut b, &[1, 2]);
    drop(b);
    clean();
}
#[test]
fn pure_directions_use_independent_counts_and_null_unused_phase() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let mut f = b.write(0, &[1, 2, 3]);
    assert_eq!(poll(&mut f), Poll::Pending);
    assert_eq!(
        (
            unsafe { async_i2c_probe_tx_len() },
            unsafe { async_i2c_probe_rx_len() },
            unsafe { async_i2c_probe_pending_rx() }
        ),
        (3, 0, 0)
    );
    complete(Ok(()));
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
    drop(f);
    let mut rx = [0; 7];
    let mut f = b.write_read(127, &[], &mut rx);
    assert_eq!(poll(&mut f), Poll::Pending);
    assert_eq!(
        (
            unsafe { async_i2c_probe_tx_len() },
            unsafe { async_i2c_probe_rx_len() },
            unsafe { async_i2c_probe_pending_tx() }
        ),
        (0, 7, 0)
    );
    complete(Ok(()));
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
    drop(f);
    assert_eq!(rx, [0x5a; 7]);
    drop(b);
    clean();
}
#[test]
fn scalar_preflight_precedes_staging_allocation() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let attempts = stats().attempts;
    unsafe { async_i2c_probe_limit(4) };
    let mut f = b.write(0x42, &[1; 5]);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::InvalidParam)));
    drop(f);
    let mut f = b.write(0x180, &[1]);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::InvalidParam)));
    drop(f);
    let mut f = b.write_read(0x42, &[], &mut []);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::InvalidParam)));
    drop(f);
    assert_eq!((stats().attempts, stats().submissions), (attempts, 0));
    drop(b);
    clean();
}
#[test]
fn empty_trait_list_does_not_flush_an_orphan() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let mut f = b.write(0x42, &[1]);
    assert_eq!(poll(&mut f), Poll::Pending);
    drop(f);
    let mut f = Box::pin(b.transaction(0x42, &mut []));
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
    drop(f);
    assert_eq!(stats().submissions, 1);
    complete(Err(Error::Io));
    let mut f = b.flush();
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::Io)));
    drop(f);
    drop(b);
    clean();
}
#[test]
fn supported_traits_share_one_native_transaction() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let tx = [1, 2, 3];
    let mut rx = [0; 7];
    watch(&tx, &mut rx);
    let mut f = Box::pin(embedded_hal_async::i2c::I2c::write_read(
        &mut b, 0x42, &tx, &mut rx,
    ));
    assert_eq!(
        f.as_mut().poll(&mut Context::from_waker(Waker::noop())),
        Poll::Pending
    );
    complete(Ok(()));
    assert_eq!(
        f.as_mut().poll(&mut Context::from_waker(Waker::noop())),
        Poll::Ready(Ok(()))
    );
    drop(f);
    drop(b);
    clean();
}
#[test]
fn asymmetric_and_zero_static_capacities() {
    let _s = SERIAL.lock().unwrap();
    reset();
    static R: AsyncI2cStorage<0, 7> = AsyncI2cStorage::new();
    let mut b = R.init(Config::default()).unwrap();
    let mut f = b.write(0x42, &[1]);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::NoSpace)));
    drop(f);
    run_read(&mut b, &mut [0; 7]);
    drop(b);
    static W: AsyncI2cStorage<3, 0> = AsyncI2cStorage::new();
    let mut b = W.init(Config::default()).unwrap();
    let mut buffer = [0];
    let mut f = b.read(0x42, &mut buffer);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::NoSpace)));
    drop(f);
    run_write(&mut b, &[1, 2, 3]);
    drop(b);
    static Z: AsyncI2cStorage<0, 0> = AsyncI2cStorage::new();
    let mut b = Z.init(Config::default()).unwrap();
    let mut f = b.write(0x42, &[1]);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::NoSpace)));
    drop(f);
    drop(b);
    assert_eq!(stats().allocations, 0);
    clean();
}
#[test]
fn independent_static_capacity_boundaries() {
    let _s = SERIAL.lock().unwrap();
    reset();
    static S: AsyncI2cStorage<3, 7> = AsyncI2cStorage::new();
    let mut b = S.init(Config::default()).unwrap();
    let mut rx = [0; 8];
    let mut f = b.write_read(0x42, &[1; 4], &mut rx[..7]);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::NoSpace)));
    drop(f);
    let mut f = b.write_read(0x42, &[1; 3], &mut rx);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::NoSpace)));
    drop(f);
    assert_eq!(stats().submissions, 0);
    let mut f = b.write_read(0x42, &[1; 3], &mut rx[..7]);
    assert_eq!(poll(&mut f), Poll::Pending);
    complete(Ok(()));
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
    drop(f);
    assert_eq!(rx[7], 0);
    drop(b);
    clean();
}
#[test]
fn static_claim_survives_pending_owner_drop() {
    let _s = SERIAL.lock().unwrap();
    reset();
    static S: AsyncI2cStorage<3, 7> = AsyncI2cStorage::new();
    let mut b = S.init(Config::default()).unwrap();
    assert!(matches!(S.init(Config::default()), Err(Error::Busy)));
    let mut f = b.write(0x42, &[1]);
    assert_eq!(poll(&mut f), Poll::Pending);
    drop(f);
    drop(b);
    assert!(matches!(S.init(Config::default()), Err(Error::Busy)));
    assert_eq!(stats().closes, 0);
    complete(Ok(()));
    let b = S.init(Config::default()).unwrap();
    drop(b);
    assert_eq!(stats().opens, 2);
    clean();
}
#[test]
fn prepare_and_partial_init_failure_release_static_claim() {
    let _s = SERIAL.lock().unwrap();
    reset();
    static S: AsyncI2cStorage<8> = AsyncI2cStorage::new();
    unsafe { async_i2c_probe_prepare_error(-5) };
    assert!(matches!(
        S.init(Config::default()),
        Err(Error::NotSupported)
    ));
    assert_eq!(stats().mutex_inits, 0);
    unsafe {
        async_i2c_probe_prepare_error(0);
        async_i2c_probe_mutex_error(-3)
    };
    assert!(matches!(S.init(Config::default()), Err(Error::NoMemory)));
    unsafe {
        async_i2c_probe_mutex_error(0);
        async_i2c_probe_open_error(-29)
    };
    assert!(matches!(S.init(Config::default()), Err(Error::Io)));
    assert_eq!((stats().mutex_inits, stats().mutex_deinits), (1, 1));
    unsafe { async_i2c_probe_open_error(0) };
    let b = S.init(Config::default()).unwrap();
    drop(b);
    clean();
}
#[test]
fn concurrent_claims_have_one_initializer() {
    let _s = SERIAL.lock().unwrap();
    reset();
    static S: AsyncI2cStorage<8> = AsyncI2cStorage::new();
    let enter = Arc::new(Barrier::new(16));
    let leave = Arc::new(Barrier::new(16));
    let winners = std::sync::atomic::AtomicUsize::new(0);
    std::thread::scope(|scope| {
        for _ in 0..16 {
            let enter = enter.clone();
            let leave = leave.clone();
            let winners = &winners;
            scope.spawn(move || {
                enter.wait();
                let result = S.init(Config::default());
                if result.is_ok() {
                    winners.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                }
                leave.wait();
                assert!(result.is_ok() || matches!(result, Err(Error::Busy)));
                drop(result);
            });
        }
    });
    assert_eq!(winners.load(std::sync::atomic::Ordering::SeqCst), 1);
    assert_eq!(stats().opens, 1);
    clean();
}
#[test]
#[cfg(not(zero_heap))]
fn native_heap_header_and_growth_failures_are_recoverable() {
    let _s = SERIAL.lock().unwrap();
    reset();
    unsafe { async_i2c_probe_fail_alloc(0) };
    assert!(matches!(
        AsyncI2c::new(Config::default()),
        Err(Error::NoMemory)
    ));
    assert_eq!(stats().mutex_inits, 0);
    unsafe { async_i2c_probe_fail_alloc(-1) };
    let mut b = bus();
    unsafe { async_i2c_probe_fail_alloc(0) };
    let mut f = b.write(0x42, &[1]);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::NoMemory)));
    drop(f);
    assert_eq!(stats().submissions, 0);
    unsafe { async_i2c_probe_fail_alloc(-1) };
    run_write(&mut b, &[1]);
    drop(b);
    clean();
}
#[test]
#[cfg(not(zero_heap))]
fn heap_growth_keeps_independent_high_water_capacities() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let header = stats().bytes;
    run_write(&mut b, &[1; 3]);
    run_read(&mut b, &mut [0; 7]);
    let allocated = stats().allocations;
    run_write(&mut b, &[1; 2]);
    run_read(&mut b, &mut [0; 4]);
    assert_eq!(stats().allocations, allocated);
    unsafe { async_i2c_probe_clear_watch() };
    let mut buffer = [0; 2];
    let mut f = b.write_read(0x42, &[1; 9], &mut buffer);
    assert_eq!(poll(&mut f), Poll::Pending);
    complete(Ok(()));
    assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
    drop(f);
    assert_eq!(stats().bytes, header + 16 + size_of::<usize>());
    assert_eq!(
        stats().peak_bytes,
        header + 10 + 16 + 2 * size_of::<usize>()
    );
    drop(b);
    clean();
}
#[test]
fn cross_core_callbacks_use_atomic_metadata_serialization() {
    let _s = SERIAL.lock().unwrap();
    reset();
    unsafe { async_i2c_probe_local_irq_model(1) };
    let mut b = bus();
    for _ in 0..32 {
        let mut rx = [0; 7];
        let mut f = b.write_read(0x42, &[1; 3], &mut rx);
        assert_eq!(poll(&mut f), Poll::Pending);
        let worker = std::thread::spawn(|| unsafe { async_i2c_probe_complete(0) });
        for _ in 0..16 {
            let _ = poll(&mut f);
            std::thread::yield_now();
        }
        worker.join().unwrap();
        assert_eq!(poll(&mut f), Poll::Ready(Ok(())));
        drop(f);
        assert_eq!(rx, [0x5a; 7]);
    }
    drop(b);
    unsafe { async_i2c_probe_local_irq_model(0) };
    clean();
}

fn rejected(bus: &mut AsyncI2c, operations: &mut [Operation<'_>], error: Error) {
    let mut future = Box::pin(bus.transaction(0x42, operations));
    let mut result = poll(&mut future);
    for _ in 0..16 {
        if result.is_ready() {
            break;
        }
        complete(Ok(()));
        result = poll(&mut future);
    }
    drop(future);
    assert_eq!(result, Poll::Ready(Err(error)));
    assert_eq!(
        stats().submissions,
        0,
        "unsupported list reached native submission"
    );
}
#[test]
fn entire_trait_shape_rejects_before_any_native_prefix() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let tx = [1, 2, 3];
    let mut a = [0; 7];
    let mut c = [0; 5];
    rejected(
        &mut b,
        &mut [Operation::Write(&tx), Operation::Write(&tx)],
        Error::NotSupported,
    );
    rejected(
        &mut b,
        &mut [Operation::Read(&mut a), Operation::Read(&mut c)],
        Error::NotSupported,
    );
    rejected(
        &mut b,
        &mut [Operation::Read(&mut a), Operation::Write(&tx)],
        Error::NotSupported,
    );
    rejected(
        &mut b,
        &mut [
            Operation::Write(&tx),
            Operation::Read(&mut a),
            Operation::Write(&tx),
        ],
        Error::NotSupported,
    );
    rejected(
        &mut b,
        &mut [
            Operation::Write(&tx),
            Operation::Read(&mut a),
            Operation::Write(&tx),
            Operation::Read(&mut c),
        ],
        Error::NotSupported,
    );
    rejected(
        &mut b,
        &mut [Operation::Write(&[]), Operation::Read(&mut a)],
        Error::NotSupported,
    );
    rejected(
        &mut b,
        &mut [Operation::Write(&tx), Operation::Read(&mut [])],
        Error::InvalidParam,
    );
    rejected(&mut b, &mut [Operation::Write(&[])], Error::InvalidParam);
    rejected(&mut b, &mut [Operation::Read(&mut [])], Error::InvalidParam);
    rejected(
        &mut b,
        &mut [Operation::Write(&[]), Operation::Read(&mut [])],
        Error::NotSupported,
    );
    rejected(
        &mut b,
        &mut [
            Operation::Write(&tx),
            Operation::Read(&mut a),
            Operation::Read(&mut []),
        ],
        Error::NotSupported,
    );
    let mut f = Box::pin(b.transaction(128, &mut []));
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::InvalidParam)));
    drop(f);
    drop(b);
    clean();
}
#[test]
fn empty_inherent_single_phase_is_invalid() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let mut b = bus();
    let mut f = b.write(0x42, &[]);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::InvalidParam)));
    drop(f);
    let mut f = b.read(0x42, &mut []);
    assert_eq!(poll(&mut f), Poll::Ready(Err(Error::InvalidParam)));
    drop(f);
    assert_eq!(stats().submissions, 0);
    drop(b);
    clean();
}
#[test]
fn static_claim_remains_reserved_through_native_deinit_return() {
    let _s = SERIAL.lock().unwrap();
    reset();
    static S: AsyncI2cStorage<8> = AsyncI2cStorage::new();
    unsafe { async_i2c_probe_hold_deinit() };
    let worker = std::thread::spawn(|| {
        let b = S.init(Config::default()).unwrap();
        drop(b);
    });
    let entered = unsafe { async_i2c_probe_wait_deinit() };
    let attempt = if entered == 1 {
        Some(S.init(Config::default()))
    } else {
        None
    };
    unsafe { async_i2c_probe_release_deinit() };
    worker.join().unwrap();
    assert_eq!(entered, 1);
    assert!(matches!(attempt, Some(Err(Error::Busy))));
    let b = S.init(Config::default()).unwrap();
    drop(b);
    clean();
}
#[test]
fn scalar_native_bridge_is_pure_and_validates_independent_phases() {
    let _s = SERIAL.lock().unwrap();
    reset();
    let b = bus();
    unsafe { async_i2c_probe_limit(7) };
    let before = stats();
    assert_eq!(unsafe { async_i2c_probe_check(0x42, 3, 7) }, 0);
    for (a, tx, rx) in [(128, 1, 0), (0x42, 0, 0), (0x42, 8, 1), (0x42, 1, 8)] {
        assert_eq!(
            unsafe { async_i2c_probe_check(a, tx, rx) },
            Error::InvalidParam.to_code()
        );
    }
    let after = stats();
    assert_eq!(
        (after.attempts, after.prepares, after.submissions),
        (before.attempts, before.prepares, before.submissions)
    );
    drop(b);
    clean();
}
#[test]
#[cfg(not(zero_heap))]
fn native_aligned_allocator_rejects_bad_layouts_and_really_frees() {
    let _s = SERIAL.lock().unwrap();
    reset();
    for (size, align) in [
        (0, 8),
        (1, 0),
        (1, 3),
        (usize::MAX, 1),
        (isize::MAX as usize, 1),
        (1, usize::MAX / 2 + 1),
    ] {
        assert!(unsafe { ove::ffi::ove_i2c_binding_alloc(size, align) }.is_null());
    }
    assert_eq!(stats().attempts, 0);
    for alignment in [1, 2, 8, 64, 256] {
        let pointer = unsafe { ove::ffi::ove_i2c_binding_alloc(17, alignment) };
        assert!(!pointer.is_null());
        assert_eq!(pointer as usize % alignment, 0);
        unsafe {
            core::ptr::write_bytes(pointer.cast::<u8>(), 0x5a, 17);
            ove::ffi::ove_i2c_binding_free(pointer)
        };
    }
    unsafe { ove::ffi::ove_i2c_binding_free(core::ptr::null_mut()) };
    assert_eq!(stats().allocations, 5);
    clean();
}

mod waker;

#[test]
#[cfg(not(zero_heap))]
fn failed_ready_growth_preserves_orphan_and_recovers() {
    let _serial = SERIAL.lock().unwrap();
    reset();
    let mut owner = bus();
    let mut rx = [0xcc; 7];
    let mut old = owner.write_read(0x42, &[1, 2, 3], &mut rx);
    assert_eq!(poll(&mut old), Poll::Pending);
    drop(old);
    complete(Err(Error::Io));
    let bytes = stats().bytes;
    unsafe { async_i2c_probe_fail_alloc(0) };
    let mut next = owner.write(0x42, &[9; 33]);
    assert_eq!(poll(&mut next), Poll::Ready(Err(Error::NoMemory)));
    drop(next);
    assert_eq!(stats().bytes, bytes);
    let mut flush = owner.flush();
    assert_eq!(poll(&mut flush), Poll::Ready(Err(Error::Io)));
    assert_eq!(poll(&mut flush), Poll::Ready(Err(Error::Io)));
    drop(flush);
    unsafe { async_i2c_probe_fail_alloc(-1) };
    run_write(&mut owner, &[9; 33]);
    assert_eq!(rx, [0xcc; 7]);
    drop(owner);
    clean();
}
#[test]
fn dropped_and_forgotten_flushes_do_not_own_pending_counts() {
    let _serial = SERIAL.lock().unwrap();
    reset();
    for forget in [false, true] {
        let mut owner = bus();
        let mut operation = owner.write(0x42, &[1]);
        assert_eq!(poll(&mut operation), Poll::Pending);
        drop(operation);
        let mut flush = owner.flush();
        assert_eq!(poll(&mut flush), Poll::Pending);
        if forget {
            core::mem::forget(flush);
        } else {
            drop(flush);
        }
        let closes = stats().closes;
        drop(owner);
        assert_eq!(stats().closes, closes);
        complete(Ok(()));
        assert_eq!(stats().closes, closes + 1);
    }
    clean();
}
#[test]
fn trait_conveniences_preserve_phase_and_empty_policies() {
    let _serial = SERIAL.lock().unwrap();
    reset();
    let mut owner = bus();
    let mut read = [0; 7];
    {
        let mut operation = Box::pin(embedded_hal_async::i2c::I2c::write(
            &mut owner,
            0x42,
            &[1, 2, 3],
        ));
        assert_eq!(poll(&mut operation), Poll::Pending);
        complete(Ok(()));
        assert_eq!(poll(&mut operation), Poll::Ready(Ok(())));
    }
    {
        let mut operation = Box::pin(embedded_hal_async::i2c::I2c::read(
            &mut owner, 0x42, &mut read,
        ));
        assert_eq!(poll(&mut operation), Poll::Pending);
        complete(Ok(()));
        assert_eq!(poll(&mut operation), Poll::Ready(Ok(())));
    }
    assert_eq!(read, [0x5a; 7]);
    let submissions = stats().submissions;
    {
        let mut operation = Box::pin(embedded_hal_async::i2c::I2c::write(&mut owner, 0x42, &[]));
        assert_eq!(poll(&mut operation), Poll::Ready(Err(Error::InvalidParam)));
    }
    {
        let mut operation = Box::pin(embedded_hal_async::i2c::I2c::read(
            &mut owner,
            0x42,
            &mut [],
        ));
        assert_eq!(poll(&mut operation), Poll::Ready(Err(Error::InvalidParam)));
    }
    assert_eq!(stats().submissions, submissions);
    drop(owner);
    clean();
}
