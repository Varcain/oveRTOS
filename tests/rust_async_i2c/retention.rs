// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//! Isolated processes account for intentionally retained owners or completions.
use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Waker};
use ove::i2c::Config;
use ove::{AsyncI2cStorage, Error};

#[repr(C)]
#[derive(Default, Debug)]
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
}

fn retain(mut owner: ove::AsyncI2c, forget_owner: bool, data: &[u8]) -> Stats {
    let mut future = owner.write(0x42, data);
    assert!(
        Pin::new(&mut future)
            .poll(&mut Context::from_waker(Waker::noop()))
            .is_pending()
    );
    core::mem::forget(future);
    if forget_owner {
        core::mem::forget(owner);
        unsafe { async_i2c_probe_complete(0) };
    } else {
        drop(owner);
    }
    let mut state = Stats::default();
    unsafe { async_i2c_probe_stats(&mut state) };
    assert_eq!(state.submissions, 1);
    assert_eq!(state.completions, usize::from(forget_owner));
    assert_eq!(state.closes, 0);
    state
}

fn main() {
    unsafe { async_i2c_probe_reset() };
    let case = std::env::args().nth(1).expect("retention case required");
    let forget = case.ends_with("forget");
    let state = if case.starts_with("static-") {
        static STORAGE: AsyncI2cStorage<8> = AsyncI2cStorage::new();
        let state = retain(STORAGE.init(Config::default()).unwrap(), forget, &[0x34; 8]);
        assert!(matches!(STORAGE.init(Config::default()), Err(Error::Busy)));
        assert_eq!(state.live, 0);
        assert_eq!(state.bytes, 0);
        state
    } else {
        #[cfg(not(zero_heap))]
        {
            assert!(case.starts_with("heap-"));
            let state = retain(
                ove::AsyncI2c::new(Config::default()).unwrap(),
                forget,
                &[0x34; 8],
            );
            assert_eq!(state.live, 2, "one header and one staging pair retained");
            assert_eq!(state.allocations, 2);
            assert_eq!(state.frees, 0);
            state
        }
        #[cfg(zero_heap)]
        panic!("heap retention case unavailable in zero-heap mode");
    };
    println!("intentional retention {case}: {state:?}");
}
