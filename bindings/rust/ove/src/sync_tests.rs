// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//! Synthetic reservation boundaries exercised through production acquisition.

use super::{Error, RecursiveMutex, RecursiveMutexStorage};
use core::sync::atomic::Ordering;
use core::time::Duration;

const RECURSIVE: u32 = 1;
const LOCK_ENTRY: u32 = 8;
const LOCK_STAGE: u32 = 2;
const RETIREMENTS: u32 = 12;
const FORBIDDEN: u32 = 5;
static SERIAL: std::sync::Mutex<()> = std::sync::Mutex::new(());

unsafe extern "C" {
    fn scoped_test_count(kind: u32, index: u32) -> u32;
    fn scoped_test_depth(kind: u32) -> u32;
    fn scoped_test_release_current() -> u32;
    fn scoped_test_gate(kind: u32, stage: u32, enabled: bool);
    fn scoped_test_wait_entered(kind: u32, stage: u32);
}

fn entries() -> u32 {
    // SAFETY: fixed valid metadata selectors for the live guarded test provider.
    unsafe { scoped_test_count(RECURSIVE, LOCK_ENTRY) }
}
fn retirements() -> u32 {
    // SAFETY: fixed valid metadata selectors for the live guarded test provider.
    unsafe { scoped_test_count(RECURSIVE, RETIREMENTS) }
}
fn forbidden() -> u32 {
    // SAFETY: fixed valid metadata selectors for the live guarded test provider.
    unsafe { scoped_test_count(RECURSIVE, FORBIDDEN) }
}
fn depth() -> u32 {
    // SAFETY: observes only this live worker's independently recorded ownership.
    unsafe { scoped_test_depth(RECURSIVE) }
}
fn retire_current() -> u32 {
    // SAFETY: called after scopes end; the guarded provider releases only live
    // acquisitions owned by this same worker, before any worker can return.
    unsafe { scoped_test_release_current() }
}
fn gate(enabled: bool) {
    // SAFETY: closed metadata selectors; no native primitive is operated here.
    unsafe { scoped_test_gate(RECURSIVE, LOCK_STAGE, enabled) }
}
fn wait_for_contender() {
    // SAFETY: a spawned contender reaches this live registry latch. The owned
    // outer controller bounds unexpected setup failures; no deadline is a pass.
    unsafe { scoped_test_wait_entered(RECURSIVE, LOCK_STAGE) }
}

fn create() -> RecursiveMutex {
    static STORAGE: RecursiveMutexStorage = RecursiveMutexStorage::new();
    RecursiveMutex::create(&STORAGE).expect("ordinary resource context")
}

// Independent safety cleanup never supplies a successful semantic observation.
// This guard also runs on setup/assertion unwind, before the owner is dropped.
struct OwnerExit<'a>(&'a RecursiveMutex);
impl OwnerExit<'_> {
    fn retire(&self) -> u32 {
        self.0.reservations.store(depth(), Ordering::Release);
        let interventions = retire_current();
        self.0.reservations.store(0, Ordering::Release);
        interventions
    }
}
impl Drop for OwnerExit<'_> {
    fn drop(&mut self) {
        self.retire();
    }
}
struct ContenderExit;
impl Drop for ContenderExit {
    fn drop(&mut self) {
        // The outer worker still owns its physical scope. Do not overwrite its
        // shared reservation metadata while retiring only this worker's debt.
        retire_current();
    }
}
struct OpenGate;
impl Drop for OpenGate {
    fn drop(&mut self) {
        gate(false);
    }
}

#[test]
fn reservation_cap_precedes_native_entry() {
    let _serial = SERIAL
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let mutex = create();
    let cleanup = OwnerExit(&mutex);
    let before_retirements = retirements();
    let before_forbidden = forbidden();
    // SAFETY: observer metadata is independent of the native primitive.
    let before = entries();
    mutex
        .reservations
        .store(RecursiveMutex::MAX_RESERVATIONS, Ordering::Release);
    let rejected = mutex.try_with_lock(|| Ok(()));
    let after_cap = entries();
    let after_rejection = mutex.reservations.load(Ordering::Acquire);

    mutex
        .reservations
        .store(RecursiveMutex::MAX_RESERVATIONS - 1, Ordering::Release);
    let mut inside = 0;
    let last_slot = mutex.try_with_lock(|| {
        inside = mutex.reservations.load(Ordering::Acquire);
        Ok(())
    });
    let after_release = mutex.reservations.load(Ordering::Acquire);
    let after_last_slot = entries();

    // Replace synthetic debt with the independently observed physical count,
    // then retire it while this worker is still live.
    let remaining_depth = depth();
    let interventions = cleanup.retire();
    drop(cleanup);
    drop(mutex);

    assert_eq!(retirements(), before_retirements);
    assert_eq!(forbidden(), before_forbidden);
    let admitted = rejected.is_ok();
    assert!(
        matches!(rejected, Ok(()) | Err(Error::Busy)),
        "cap probe returns only admission or the reservation rejection"
    );
    assert_eq!(
        after_cap,
        before + u32::from(admitted),
        "native entry agrees with the observed cap admission"
    );
    assert_eq!(after_rejection, RecursiveMutex::MAX_RESERVATIONS);
    assert!(last_slot.is_ok(), "last reservation admitted");
    assert_eq!(inside, RecursiveMutex::MAX_RESERVATIONS);
    assert_eq!(after_release, RecursiveMutex::MAX_RESERVATIONS - 1);
    assert_eq!(after_last_slot, after_cap + 1);
    assert_eq!(
        (remaining_depth, interventions),
        (0, 0),
        "physical release completed"
    );
    // Complete every positive/control observation before the deliberate
    // discriminator, so its exact failure cannot hide an unrelated error.
    assert!(
        matches!(rejected, Err(Error::Busy)),
        "reservation cap rejected"
    );
}

#[test]
fn pending_reservation_rolls_back_after_timeout() {
    let _serial = SERIAL
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let mutex = create();
    let cleanup = OwnerExit(&mutex);
    let before_retirements = retirements();
    let before_forbidden = forbidden();
    let before = entries();
    let mut pending_rejected = false;
    let mut cap_did_not_enter = false;
    let mut timed_out = false;
    let mut rollback = false;
    let outer = mutex.with_lock(|| {
        // One real held scope plus synthetic reservations leaves one slot.
        mutex
            .reservations
            .store(RecursiveMutex::MAX_RESERVATIONS - 1, Ordering::Release);
        gate(true);
        std::thread::scope(|scope| {
            let _gate = OpenGate;
            let contender = std::thread::Builder::new()
                .stack_size(64 * 1024 * 1024)
                .spawn_scoped(scope, || {
                    let _exit = ContenderExit;
                    mutex.try_with_lock_for(Duration::from_millis(10), || Ok(()))
                })
                .expect("bounded contender spawn");
            wait_for_contender();
            let entered = entries();
            pending_rejected = matches!(mutex.try_with_lock(|| Ok(())), Err(Error::Busy));
            cap_did_not_enter = entries() == entered;
            gate(false);
            timed_out = matches!(contender.join(), Ok(Err(Error::Timeout)));
        });
        rollback =
            mutex.reservations.load(Ordering::Acquire) == RecursiveMutex::MAX_RESERVATIONS - 1;
        // The joined contender holds nothing; only this real scope remains.
        mutex.reservations.store(depth(), Ordering::Release);
        Ok(())
    });
    let final_count = mutex.reservations.load(Ordering::Acquire);
    let debt = cleanup.retire();
    let reuse = mutex.try_with_lock(|| Ok(()));
    let final_entries = entries();
    let remaining_depth = depth();
    let final_debt = cleanup.retire();
    drop(cleanup);
    drop(mutex);

    assert_eq!(retirements(), before_retirements);
    assert_eq!(forbidden(), before_forbidden);
    assert!(outer.is_ok());
    assert!(
        pending_rejected && cap_did_not_enter,
        "pending reservation occupies capacity"
    );
    assert!(
        timed_out,
        "native contender times out while its owner is held"
    );
    assert_eq!(
        (final_count, debt, remaining_depth, final_debt),
        (0, 0, 0, 0)
    );
    assert!(reuse.is_ok());
    assert_eq!(final_entries, before + 3);
    assert!(
        rollback,
        "failed native acquisition rolls back its reservation"
    );
}
