// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#![no_std]

#[cfg(zero_heap)]
static MUTEX: ove::MutexStorage = ove::MutexStorage::new();
#[cfg(zero_heap)]
static RECURSIVE: ove::RecursiveMutexStorage = ove::RecursiveMutexStorage::new();
#[cfg(zero_heap)]
static SEMAPHORE: ove::SemaphoreStorage = ove::SemaphoreStorage::new();
#[cfg(zero_heap)]
static EVENT: ove::EventStorage = ove::EventStorage::new();
#[cfg(zero_heap)]
static CONDVAR: ove::CondVarStorage = ove::CondVarStorage::new();

fn constructors() -> ove::Result<()> {
    #[cfg(not(zero_heap))]
    let (_m, _r, _s, _e, _c) = (
        ove::Mutex::new(0u32)?,
        ove::RecursiveMutex::new()?,
        ove::Semaphore::new(0, 2)?,
        ove::Event::new()?,
        ove::CondVar::new()?,
    );
    #[cfg(zero_heap)]
    let (_m, _r, _s, _e, _c) = (
        ove::Mutex::create(&MUTEX, 0u32)?,
        ove::RecursiveMutex::create(&RECURSIVE)?,
        ove::Semaphore::create(&SEMAPHORE, 0, 2)?,
        ove::Event::create(&EVENT)?,
        ove::CondVar::create(&CONDVAR)?,
    );
    Ok(())
}

#[unsafe(no_mangle)]
pub extern "C" fn sync_binding_compile_consumer() -> i32 {
    if constructors().is_ok() { 0 } else { 1 }
}

#[ove::main]
fn app_main() {
    ove::run();
}
