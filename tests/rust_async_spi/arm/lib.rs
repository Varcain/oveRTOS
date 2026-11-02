// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//! Real ARM kernel/executor fixture without a Rust global allocator.
#![no_std]

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Waker};
use ove::AsyncSpiStorage;
use ove::spi::Config;

mod consumer {
    include!("../consumer.rs");
}
unsafe extern "C" {
    fn rust_spi_ready();
    fn rust_spi_finished();
    fn rust_spi_panic() -> !;
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { rust_spi_panic() }
}

#[embassy_executor::task]
async fn transfers() {
    consumer::public_async_spi_consumer().await.unwrap();
    let mut heap = ove::AsyncSpi16::new(Config::default()).unwrap();
    heap.write(None, &[0x1234]).await.unwrap();
    let mut rx = [0; 257];
    heap.transfer(None, &[0xabcd; 257], &mut rx).await.unwrap();
    assert_eq!(rx, [0xabcd; 257]);
    drop(heap);

    static CANCELED: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut owner = CANCELED
        .init(Config {
            instance: 1,
            ..Config::default()
        })
        .unwrap();
    let mut future = owner.write(None, &[1; 8]);
    assert!(
        Pin::new(&mut future)
            .poll(&mut Context::from_waker(Waker::noop()))
            .is_pending()
    );
    core::mem::forget(future);
    drop(owner);

    let mut owner = ove::AsyncSpi16::new(Config {
        instance: 2,
        ..Config::default()
    })
    .unwrap();
    let mut future = owner.write(None, &[0x1234; 257]);
    assert!(
        Pin::new(&mut future)
            .poll(&mut Context::from_waker(Waker::noop()))
            .is_pending()
    );
    drop(future);
    drop(owner);
    unsafe { rust_spi_finished() };
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_spi_run() -> ! {
    let executor = ove::Executor::take().unwrap();
    executor.run(|spawner| {
        unsafe { rust_spi_ready() };
        spawner.must_spawn(transfers());
    })
}
