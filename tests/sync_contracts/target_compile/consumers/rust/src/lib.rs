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
    _s.release_n(0);
    let _ = _s.try_acquire();
    _e.signal();
    _e.signal_from_isr();
    let _ = _e.try_wait();
    let _macro_mutex = ove::mutex!(0u32);
    let _macro_recursive = ove::recursive_mutex!();
    let _macro_semaphore = ove::semaphore!(0, 2);
    let _macro_event = ove::event!();
    let _macro_cv = ove::condvar!();
    Ok(())
}

// Retained target data exposes actual native and binding layout without running
// constructors, querying context, or allocating an owner.
#[unsafe(no_mangle)]
pub static sync_binding_storage_values: [usize; 12] = [
    core::mem::size_of::<ove::ffi::ove_sem_storage_t>(),
    core::mem::align_of::<ove::ffi::ove_sem_storage_t>(),
    core::mem::size_of::<ove::SemaphoreStorage>(),
    core::mem::align_of::<ove::SemaphoreStorage>(),
    core::mem::size_of::<ove::Semaphore>(),
    core::mem::align_of::<ove::Semaphore>(),
    core::mem::size_of::<ove::ffi::ove_event_storage_t>(),
    core::mem::align_of::<ove::ffi::ove_event_storage_t>(),
    core::mem::size_of::<ove::EventStorage>(),
    core::mem::align_of::<ove::EventStorage>(),
    core::mem::size_of::<ove::Event>(),
    core::mem::align_of::<ove::Event>(),
];

#[unsafe(no_mangle)]
pub extern "C" fn sync_binding_compile_consumer() -> i32 {
    if constructors().is_ok() { 0 } else { 1 }
}

#[ove::main]
fn app_main() {
    ove::run();
}
