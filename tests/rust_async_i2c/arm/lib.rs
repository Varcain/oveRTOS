// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//! Actual ARM kernel/executor fixture without a Rust global allocator.
#![no_std]
use core::{
    future::Future,
    pin::Pin,
    task::{Context, Waker},
};
use ove::{AsyncI2cStorage, i2c::Config};
mod consumer {
    include!("../consumer.rs");
}
unsafe extern "C" {
    fn rust_i2c_ready();
    fn rust_i2c_finished();
    fn rust_i2c_panic() -> !;
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { rust_i2c_panic() }
}
#[embassy_executor::task]
async fn transfers() {
    consumer::public_async_i2c_consumer().await.unwrap();
    let mut heap = ove::AsyncI2c::new(Config::default()).unwrap();
    let mut rx = [0; 257];
    heap.write_read(0x42, &[1, 2, 3], &mut rx).await.unwrap();
    assert_eq!(rx, [0x5a; 257]);
    drop(heap);
    static CANCELED: AsyncI2cStorage<3, 7> = AsyncI2cStorage::new();
    let mut owner = CANCELED
        .init(Config {
            instance: 1,
            ..Config::default()
        })
        .unwrap();
    let mut rx = [0xcc; 7];
    let mut future = owner.write_read(0x42, &[1, 2, 3], &mut rx);
    assert!(
        Pin::new(&mut future)
            .poll(&mut Context::from_waker(Waker::noop()))
            .is_pending()
    );
    core::mem::forget(future);
    drop(owner);
    assert_eq!(rx, [0xcc; 7]);
    let mut owner = ove::AsyncI2c::new(Config {
        instance: 2,
        ..Config::default()
    })
    .unwrap();
    let mut future = owner.write(0x42, &[9; 257]);
    assert!(
        Pin::new(&mut future)
            .poll(&mut Context::from_waker(Waker::noop()))
            .is_pending()
    );
    drop(future);
    drop(owner);
    unsafe { rust_i2c_finished() };
}
#[unsafe(no_mangle)]
pub extern "C" fn rust_i2c_run() -> ! {
    ove::Executor::take().unwrap().run(|spawner| {
        unsafe { rust_i2c_ready() };
        spawner.must_spawn(transfers());
    })
}
