// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#![no_std]

#[unsafe(no_mangle)]
pub extern "C" fn sync_binding_context_flags() -> u32 {
    // The query has no primitive, pointer, ownership or allocation prerequisite.
    unsafe { ove::ffi::ove_sync_context() }
}

/// # Safety
/// The native caller must satisfy the checked wait's handle, ownership,
/// context, output and backing-lifetime requirements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sync_binding_checked_wait(
    cv: ove::ffi::ove_condvar_t,
    mutex: ove::ffi::ove_mutex_t,
    timeout: u64,
    held: *mut bool,
) -> i32 {
    // The compile-only native ABI caller supplies the checked wait's contract.
    unsafe { ove::ffi::ove_condvar_wait_checked(cv, mutex, timeout, held) }
}

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
    _m.with_lock(|mut access| {
        *access += 1;
        let (_access, _timeout) = _c.wait_for(access, core::time::Duration::ZERO)?;
        Ok(())
    })?;
    _r.with_lock(|| Ok(()))?;
    let _macro_mutex = ove::mutex!(0u32);
    let _macro_recursive = ove::recursive_mutex!();
    let _macro_cv = ove::condvar!();
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
