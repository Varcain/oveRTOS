// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#![no_std]

use ove::{
    CondVar, CondVarStorage, InitCell, Mutex, MutexStorage, RecursiveMutex, RecursiveMutexStorage,
};

static M_STORAGE: MutexStorage = MutexStorage::new();
static R_STORAGE: RecursiveMutexStorage = RecursiveMutexStorage::new();
static C_STORAGE: CondVarStorage = CondVarStorage::new();
static M: InitCell<Mutex<()>> = InitCell::new();
static R: InitCell<RecursiveMutex> = InitCell::new();
static C: InitCell<CondVar> = InitCell::new();

unsafe extern "C" {
    fn scoped_native_start();
    fn scoped_native_reject() -> !;
    fn scoped_native_probe(kind: u32);
}

fn probe(kind: u32) {
    // Observe native current-worker ownership without creating a data reference.
    // A missing owner refuses immediately, before a possible invalid release.
    unsafe { scoped_native_probe(kind) }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    // A panic is refusal, never an expected passing ownership observation.
    unsafe { scoped_native_reject() }
}

#[unsafe(no_mangle)]
pub extern "C" fn scoped_native_initialize() -> i32 {
    fn init() -> ove::Result<()> {
        // C starts no application worker until this single-threaded phase ends.
        M.init(Mutex::create(&M_STORAGE, ())?);
        R.init(RecursiveMutex::create(&R_STORAGE)?);
        C.init(CondVar::create(&C_STORAGE)?);
        Ok(())
    }
    if init().is_ok() { 0 } else { 1 }
}

#[unsafe(no_mangle)]
pub extern "C" fn scoped_native_step(index: u32) -> i32 {
    fn step(index: u32) -> ove::Result<()> {
        let mutex = M.try_get().ok_or(ove::Error::NotRegistered)?;
        let recursive = R.try_get().ok_or(ove::Error::NotRegistered)?;
        if index == 0 {
            mutex.with_lock(|access| {
                probe(0);
                core::mem::forget(access);
                probe(0);
                Ok(())
            })?;
        } else {
            let cv = C.try_get().ok_or(ove::Error::NotRegistered)?;
            mutex.with_lock(|access| {
                probe(0);
                let (access, outcome) = cv.wait_for(access, core::time::Duration::ZERO)?;
                if !outcome.timed_out() {
                    return Err(ove::Error::Io);
                }
                probe(0);
                core::mem::forget(access);
                probe(0);
                Ok(())
            })?;
        }
        recursive.with_lock(|| {
            probe(1);
            recursive.with_lock(|| {
                probe(1);
                Ok(())
            })?;
            probe(1);
            Ok(())
        })
    }
    if step(index).is_ok() { 0 } else { 1 }
}

#[unsafe(no_mangle)]
pub extern "C" fn scoped_native_shutdown() {
    // C has joined or retired both workers; this lifecycle phase is exclusive.
    C.shutdown();
    R.shutdown();
    M.shutdown();
}

#[ove::main]
fn app_main() {
    // The C test controller owns kernel worker storage, readiness and retirement.
    unsafe { scoped_native_start() }
}
