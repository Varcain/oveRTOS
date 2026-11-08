// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(non_camel_case_types)]

mod bindings {
    pub type ove_irq_key_t = u64;
    unsafe extern "C" {
        pub fn ove_irq_lock() -> ove_irq_key_t;
        pub fn ove_irq_unlock(key: ove_irq_key_t);
    }
}
include!(env!("OVE_SPIN_SOURCE"));

unsafe extern "C" {
    fn spin_depth() -> u32;
    fn spin_mask() -> u32;
    fn spin_violations() -> u32;
    fn spin_interventions() -> u32;
    fn spin_cleanup();
    fn spin_real_enabled() -> u32;
}
use std::sync::atomic::{AtomicU32, Ordering};
static CHECKS: AtomicU32 = AtomicU32::new(0);
static FAILURES: AtomicU32 = AtomicU32::new(0);
static CASES: AtomicU32 = AtomicU32::new(0);

fn check(ok: bool, message: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if !ok {
        FAILURES.fetch_add(1, Ordering::Relaxed);
        println!("FAIL: {message}");
    }
}

fn admission() {
    CASES.fetch_add(1, Ordering::Relaxed);
    let owner = SpinMutex::new(17u32);
    let first = spin_mutex::admit(&owner);
    check(first.is_some(), "first reference-free acquisition succeeds");
    let second = spin_mutex::admit(&owner);
    check(
        second.is_none(),
        "second reference-free acquisition is Busy",
    );
    #[cfg(not(ove_spin_parent))]
    check(
        spin_mutex::claimed(&owner),
        "failed acquisition retains outer claim",
    );
    let third = spin_mutex::admit(&owner);
    check(
        third.is_none(),
        "third reference-free acquisition remains Busy",
    );
    #[cfg(not(ove_spin_parent))]
    check(
        spin_mutex::claimed(&owner),
        "another failure cannot release outer claim",
    );
    // Every actually returned lease is retired LIFO, even on broken admission.
    drop(third);
    drop(second);
    drop(first);
    check(
        unsafe { spin_depth() } == 0,
        "all returned keys retire before reuse",
    );
    let fresh = spin_mutex::admit(&owner);
    check(fresh.is_some(), "true owner release reopens admission");
    drop(fresh);
    check(
        unsafe { spin_depth() } == 0 && unsafe { spin_mask() } == 37,
        "key depth and original state restore exactly",
    );
    unsafe { spin_cleanup() };
}

fn callbacks() {
    CASES.fetch_add(1, Ordering::Relaxed);
    let mut owner = SpinMutex::new(10u32);
    check(
        owner.with(|value| {
            *value += 2;
            *value
        }) == 12,
        "callback mutation and scalar result",
    );
    let result: Result<u32, u8> = owner.with(|_| Err(7));
    check(
        result == Err(7),
        "application Result passes through unchanged",
    );
    check(
        unsafe { spin_depth() } == 0,
        "return retires the private native key",
    );
    *owner.get_mut() += 3;
    check(
        owner.into_inner() == 15,
        "exclusive owner access and into_inner remain available",
    );

    CASES.fetch_add(1, Ordering::Relaxed);
    let a = SpinMutex::new(1u32);
    let b = SpinMutex::new(2u32);
    a.with(|left| {
        b.with(|right| {
            *left += *right;
            check(
                unsafe { spin_depth() } == 2,
                "distinct scopes retain nested native keys",
            );
        });
        check(
            unsafe { spin_depth() } == 1,
            "inner completion preserves the outer key",
        );
    });
    check(
        a.into_inner() == 3 && b.into_inner() == 2,
        "distinct object views remain disjoint",
    );

    CASES.fetch_add(1, Ordering::Relaxed);
    let owner = SpinMutex::new(8u32);
    let caught = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        owner.with(|value| {
            *value += 1;
            panic!("controlled callback unwind");
        });
    }));
    check(
        caught.is_err() && unsafe { spin_depth() } == 0,
        "callback unwind retires the private lease",
    );
    check(
        owner.with(|value| *value) == 9,
        "callback unwind reopens admission",
    );

    CASES.fetch_add(1, Ordering::Relaxed);
    static STATIC: SpinMutex<u32> = SpinMutex::new(1);
    check(
        STATIC.with(|value| {
            *value += 1;
            *value
        }) == 2,
        "const static construction",
    );
    let owner: Box<SpinMutex<[u8]>> = Box::new(SpinMutex::new([1u8, 2, 3]));
    check(
        owner.with(|slice| {
            slice[1] = 7;
            slice.len()
        }) == 3,
        "unsized payload access",
    );
    let mut borrowed = 4u32;
    let owner = SpinMutex::new(&mut borrowed);
    owner.with(|value| **value += 1);
    check(*owner.into_inner() == 5, "borrowed non-static payload");

    CASES.fetch_add(1, Ordering::Relaxed);
    let owner = SpinMutex::new(4u32);
    #[allow(forgetting_references)]
    owner.with(|value| std::mem::forget(value));
    check(
        unsafe { spin_depth() } == 0,
        "forgetting a callback borrow cannot own cleanup",
    );
    std::mem::forget(owner);
    check(
        unsafe { spin_depth() } == 0,
        "forgetting an idle scalar owner owes no native key",
    );

    CASES.fetch_add(1, Ordering::Relaxed);
    struct Counted(std::sync::Arc<AtomicU32>);
    impl Drop for Counted {
        fn drop(&mut self) {
            self.0.fetch_add(1, Ordering::Relaxed);
        }
    }
    let count = std::sync::Arc::new(AtomicU32::new(0));
    let owner = SpinMutex::new(Counted(count.clone()));
    owner.with(|_| ());
    drop(owner.into_inner());
    check(
        count.load(Ordering::Relaxed) == 1,
        "payload destruction happens exactly once",
    );
    check(
        unsafe { spin_depth() } == 0 && unsafe { spin_mask() } == 37,
        "ordinary callbacks restore the original state",
    );
    unsafe { spin_cleanup() };
}

fn threads() {
    use std::sync::{Arc, Barrier};
    CASES.fetch_add(1, Ordering::Relaxed);
    let owner = Arc::new(SpinMutex::new(0u32));
    if unsafe { spin_real_enabled() } != 0 {
        let ready = Arc::new(Barrier::new(2));
        let workers: Vec<_> = (0..2)
            .map(|_| {
                let (owner, ready) = (owner.clone(), ready.clone());
                std::thread::spawn(move || {
                    ready.wait();
                    for _ in 0..32 {
                        owner.with(|value| *value += 1);
                    }
                    check(
                        unsafe { spin_depth() } == 0,
                        "real POSIX worker retires before exit",
                    );
                    unsafe { spin_cleanup() };
                })
            })
            .collect();
        for worker in workers {
            worker.join().expect("ordinary worker completes");
        }
        check(
            owner.with(|value| *value) == 64,
            "actual POSIX exclusion serializes ordinary data access",
        );
    } else {
        // Contention on the deliberately nonserializing model is reference-free.
        let first = spin_mutex::admit(&*owner);
        let other = owner.clone();
        let worker = std::thread::spawn(move || {
            let second = spin_mutex::admit(&*other);
            check(
                second.is_none(),
                "another modeled worker cannot claim the same data",
            );
            drop(second);
            check(
                unsafe { spin_depth() } == 0,
                "rejected worker balances only its own key",
            );
            unsafe { spin_cleanup() };
        });
        worker.join().expect("reference-free contender completes");
        drop(first);
        let other = owner.clone();
        std::thread::spawn(move || {
            let fresh = spin_mutex::admit(&*other);
            check(
                fresh.is_some(),
                "later worker acquires after true owner release",
            );
            drop(fresh);
            unsafe { spin_cleanup() };
        })
        .join()
        .expect("fresh worker completes");
    }
    check(
        unsafe { spin_depth() } == 0,
        "controller holds no native debt after joins",
    );
    unsafe { spin_cleanup() };
}

fn main() {
    std::panic::set_hook(Box::new(|_| {}));
    let mode = std::env::args().nth(1).expect("normal or canary selector");
    assert!(
        mode == "normal" || mode == "canary" || mode == "admission" || mode == "admission-canary"
    );
    if mode.ends_with("canary") {
        check(
            false,
            "early failure survives every later ownership control",
        );
    }
    admission();
    if !mode.starts_with("admission") {
        callbacks();
        threads();
    }
    check(
        unsafe { spin_violations() } == 0,
        "no invalid native restore was attempted",
    );
    check(
        unsafe { spin_interventions() } == 0,
        "no independent cleanup was needed",
    );
    println!(
        "spinmutex ownership: cases={} checks={} failures={}",
        CASES.load(Ordering::Relaxed),
        CHECKS.load(Ordering::Relaxed),
        FAILURES.load(Ordering::Relaxed)
    );
    std::process::exit(i32::from(FAILURES.load(Ordering::Relaxed) != 0));
}
