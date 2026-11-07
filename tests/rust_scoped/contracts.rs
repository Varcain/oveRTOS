// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//! Guarded native forwarding; parent hazardous paths use unit payloads only.

use core::sync::atomic::{AtomicU32, Ordering};
use core::time::Duration;
#[cfg(zero_heap)]
use core::{cell::UnsafeCell, mem::MaybeUninit};
use ove::{
    CondVar, CondVarStorage, Error, Mutex, MutexStorage, RecursiveMutex, RecursiveMutexStorage,
};

const MUTEX: u32 = 0;
const RECURSIVE: u32 = 1;
const CONDVAR: u32 = 2;
const INIT: u32 = 0;
const CREATE: u32 = 1;
const DEINIT: u32 = 2;
const DESTROY: u32 = 3;
const DUPLICATE: u32 = 4;
const FORBIDDEN: u32 = 5;
const NATIVE_INIT: u32 = 6;
const NATIVE_DEINIT: u32 = 7;
const LOCK: u32 = 8;
const UNLOCK: u32 = 9;
const WAIT: u32 = 10;
const NOTIFY: u32 = 11;
const RETIRE: u32 = 12;
const INIT_STAGE: u32 = 0;
const DEINIT_STAGE: u32 = 1;
const WAIT_RETURN_STAGE: u32 = 3;
const RESOURCE: u32 = 1;
const BLOCKING: u32 = 2;
static FAILURES: AtomicU32 = AtomicU32::new(0);
static CHECKS: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" {
    fn scoped_test_context(flags: u32, isr: i32);
    fn scoped_test_count(kind: u32, index: u32) -> u32;
    fn scoped_test_live(kind: u32) -> u32;
    fn scoped_test_depth(kind: u32) -> u32;
    fn scoped_test_release_current() -> u32;
    fn scoped_test_cleanup_retained() -> u32;
    fn scoped_test_registry_ok() -> bool;
    fn scoped_test_failure(kind: u32, mode: u32);
    fn scoped_test_gate(kind: u32, stage: u32, enabled: bool);
    fn scoped_test_wait_entered(kind: u32, stage: u32);
    fn scoped_test_wait_registered();
    fn scoped_test_recursive_model(enabled: bool);
    fn scoped_test_wait_model(mode: u32);
    #[cfg(has_time)]
    fn scoped_test_clock_calls() -> u32;
    fn scoped_test_actual_critical_depth() -> u32;
    fn posix_irq_enter_isr();
    fn posix_irq_leave_isr();
}

fn check(valid: bool, message: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if !valid {
        FAILURES.fetch_add(1, Ordering::Relaxed);
        eprintln!("failed: {message}");
    }
}
fn required<T>(value: ove::Result<T>) -> Option<T> {
    match value {
        Ok(value) => Some(value),
        Err(_) => {
            check(false, "ordinary constructor succeeds");
            None
        }
    }
}
fn count(kind: u32, index: u32) -> u32 {
    // SAFETY: every caller uses the closed provider metadata enum above.
    unsafe { scoped_test_count(kind, index) }
}
fn live(kind: u32) -> u32 {
    // SAFETY: a closed kind selector observes persistent registry metadata.
    unsafe { scoped_test_live(kind) }
}
fn depth(kind: u32) -> u32 {
    // SAFETY: the provider observes only this still-live worker's ownership.
    unsafe { scoped_test_depth(kind) }
}
fn context(flags: u32, isr: i32) {
    // SAFETY: changes synthetic metadata on this ordinary host thread only.
    unsafe { scoped_test_context(flags, isr) }
}
fn normal_context() {
    context(u32::MAX, -1);
}
fn gate(kind: u32, stage: u32, enabled: bool) {
    // SAFETY: closed live registry selectors; no native primitive effects.
    unsafe { scoped_test_gate(kind, stage, enabled) }
}
fn wait_entered(kind: u32, stage: u32) {
    // SAFETY: called after spawning the selected visitor; outer owned deadline
    // bounds unexpected setup failure, which can never satisfy a passing case.
    unsafe { scoped_test_wait_entered(kind, stage) }
}
fn retire_current() -> u32 {
    // SAFETY: called after public capabilities are unreachable and scopes ended,
    // before this worker returns. Provider validates and releases its own levels.
    unsafe { scoped_test_release_current() }
}
struct WorkerExit;
impl Drop for WorkerExit {
    fn drop(&mut self) {
        let debt = retire_current();
        check(debt == 0, "worker return has no native acquisition debt");
    }
}

#[cfg(scoped_parent)]
trait ParentMutexScopes<T: ?Sized> {
    fn with_lock<R>(
        &self,
        f: impl for<'s> FnOnce(ove::MutexGuard<'s, T>) -> ove::Result<R>,
    ) -> ove::Result<R>;
    fn try_with_lock<R>(
        &self,
        f: impl for<'s> FnOnce(ove::MutexGuard<'s, T>) -> ove::Result<R>,
    ) -> ove::Result<R>;
    fn try_with_lock_for<R>(
        &self,
        d: Duration,
        f: impl for<'s> FnOnce(ove::MutexGuard<'s, T>) -> ove::Result<R>,
    ) -> ove::Result<R>;
    #[cfg(has_time)]
    fn try_with_lock_until<R>(
        &self,
        d: ove::time::Instant,
        f: impl for<'s> FnOnce(ove::MutexGuard<'s, T>) -> ove::Result<R>,
    ) -> ove::Result<R>;
}
#[cfg(scoped_parent)]
impl<T: ?Sized> ParentMutexScopes<T> for Mutex<T> {
    fn with_lock<R>(
        &self,
        f: impl for<'s> FnOnce(ove::MutexGuard<'s, T>) -> ove::Result<R>,
    ) -> ove::Result<R> {
        f(self.lock()?)
    }
    fn try_with_lock<R>(
        &self,
        f: impl for<'s> FnOnce(ove::MutexGuard<'s, T>) -> ove::Result<R>,
    ) -> ove::Result<R> {
        f(self.try_lock()?)
    }
    fn try_with_lock_for<R>(
        &self,
        d: Duration,
        f: impl for<'s> FnOnce(ove::MutexGuard<'s, T>) -> ove::Result<R>,
    ) -> ove::Result<R> {
        f(self.try_lock_for(d)?)
    }
    #[cfg(has_time)]
    fn try_with_lock_until<R>(
        &self,
        d: ove::time::Instant,
        f: impl for<'s> FnOnce(ove::MutexGuard<'s, T>) -> ove::Result<R>,
    ) -> ove::Result<R> {
        f(self.try_lock_until(d)?)
    }
}

trait Ops: 'static {
    type Storage: Sync;
    type Owner: Send + Sync;
    const KIND: u32;
    fn storage(index: usize) -> &'static Self::Storage;
    fn create(storage: &'static Self::Storage) -> ove::Result<Self::Owner>;
    fn macro_owner() -> Self::Owner;
    #[cfg(zero_heap)]
    unsafe fn raw() -> ove::Result<Self::Owner>;
}
struct M;
struct R;
struct C;
impl Ops for M {
    type Storage = MutexStorage;
    type Owner = Mutex<()>;
    const KIND: u32 = MUTEX;
    fn storage(index: usize) -> &'static MutexStorage {
        static STORAGE: [MutexStorage; 8] = [const { MutexStorage::new() }; 8];
        &STORAGE[index]
    }
    fn create(storage: &'static MutexStorage) -> ove::Result<Mutex<()>> {
        Mutex::create(storage, ())
    }
    fn macro_owner() -> Mutex<()> {
        ove::mutex!(())
    }
    #[cfg(zero_heap)]
    unsafe fn raw() -> ove::Result<Mutex<()>> {
        static RAW: RawCell<ove::ffi::ove_mutex_storage_t> = RawCell::new();
        // SAFETY: closed case gives this stable raw cell one exclusive owner.
        unsafe { Mutex::from_static(RAW.0.get().cast(), ()) }
    }
}
impl Ops for R {
    type Storage = RecursiveMutexStorage;
    type Owner = RecursiveMutex;
    const KIND: u32 = RECURSIVE;
    fn storage(index: usize) -> &'static RecursiveMutexStorage {
        static STORAGE: [RecursiveMutexStorage; 8] = [const { RecursiveMutexStorage::new() }; 8];
        &STORAGE[index]
    }
    fn create(storage: &'static RecursiveMutexStorage) -> ove::Result<RecursiveMutex> {
        RecursiveMutex::create(storage)
    }
    fn macro_owner() -> RecursiveMutex {
        ove::recursive_mutex!()
    }
    #[cfg(zero_heap)]
    unsafe fn raw() -> ove::Result<RecursiveMutex> {
        static RAW: RawCell<ove::ffi::ove_mutex_storage_t> = RawCell::new();
        // SAFETY: independent, stable, complete raw backing used once at a time.
        unsafe { RecursiveMutex::from_static(RAW.0.get().cast()) }
    }
}
impl Ops for C {
    type Storage = CondVarStorage;
    type Owner = CondVar;
    const KIND: u32 = CONDVAR;
    fn storage(index: usize) -> &'static CondVarStorage {
        static STORAGE: [CondVarStorage; 8] = [const { CondVarStorage::new() }; 8];
        &STORAGE[index]
    }
    fn create(storage: &'static CondVarStorage) -> ove::Result<CondVar> {
        CondVar::create(storage)
    }
    fn macro_owner() -> CondVar {
        ove::condvar!()
    }
    #[cfg(zero_heap)]
    unsafe fn raw() -> ove::Result<CondVar> {
        static RAW: RawCell<ove::ffi::ove_condvar_storage_t> = RawCell::new();
        // SAFETY: independent stable raw backing, no typed owner aliases it.
        unsafe { CondVar::from_static(RAW.0.get().cast()) }
    }
}
#[cfg(zero_heap)]
struct RawCell<T>(UnsafeCell<MaybeUninit<T>>);
#[cfg(zero_heap)]
impl<T> RawCell<T> {
    const fn new() -> Self {
        Self(UnsafeCell::new(MaybeUninit::zeroed()))
    }
}
// SAFETY: selected raw-origin cases admit one owner and finish cleanup before reuse.
#[cfg(zero_heap)]
unsafe impl<T> Sync for RawCell<T> {}

fn controls<O: Ops>() {
    let Some(a) = required(O::create(O::storage(0))) else {
        return;
    };
    let Some(b) = required(O::create(O::storage(1))) else {
        drop(a);
        return;
    };
    check(live(O::KIND) >= 2, "independent native backings are live");
    drop(b);
    drop(a);
    check(
        unsafe { scoped_test_registry_ok() },
        "registry generations and live pointers agree",
    );
}
fn duplicate<O: Ops>() {
    let Some(owner) = required(O::create(O::storage(2))) else {
        return;
    };
    let before = count(O::KIND, INIT);
    let second = O::create(O::storage(2));
    #[cfg(zero_heap)]
    {
        check(
            matches!(second, Err(Error::Busy)),
            "live static claim rejects a second owner",
        );
        check(
            count(O::KIND, INIT) == before,
            "duplicate create stops before native init",
        );
    }
    #[cfg(not(zero_heap))]
    {
        check(second.is_ok(), "heap create ignores unused static storage");
        check(
            count(O::KIND, INIT) == before + 1,
            "heap owners have independent native init",
        );
    }
    drop(second);
    drop(owner);
}
fn overlap<O: Ops>(stage: u32) {
    gate(O::KIND, stage, true);
    let owner = if stage == DEINIT_STAGE {
        required(O::create(O::storage(2)))
    } else {
        None
    };
    std::thread::scope(|scope| {
        let worker = std::thread::Builder::new()
            .stack_size(64 * 1024 * 1024)
            .spawn_scoped(scope, move || {
                let _exit = WorkerExit;
                if let Some(owner) = owner {
                    drop(owner);
                    None
                } else {
                    required(O::create(O::storage(2)))
                }
            })
            .expect("bounded lifecycle worker");
        wait_entered(O::KIND, stage);
        let before = count(O::KIND, INIT);
        let visitor = O::create(O::storage(2));
        #[cfg(zero_heap)]
        {
            check(
                matches!(visitor, Err(Error::Busy)),
                "claim excludes an overlapping lifecycle visitor",
            );
            check(
                count(O::KIND, INIT) == before,
                "overlap stops before native initializer",
            );
        }
        #[cfg(not(zero_heap))]
        {
            check(
                visitor.is_ok(),
                "independent heap lifecycle visitor succeeds",
            );
            check(
                count(O::KIND, INIT) == before + 1,
                "independent heap visitor initializes separately",
            );
        }
        drop(visitor);
        gate(O::KIND, stage, false);
        match worker.join() {
            Ok(result) => drop(result),
            Err(_) => check(false, "lifecycle worker completed normally"),
        }
    });
}
fn failed_init<O: Ops>(mode: u32) {
    // SAFETY: closed one-shot provider-error mode, all real initialized pieces
    // are cleaned before an error is reported. There is no invented live owner.
    unsafe { scoped_test_failure(O::KIND, mode) };
    let result = O::create(O::storage(2));
    check(
        matches!(result, Err(Error::NoMemory)),
        "cleaned native initialization error propagates",
    );
    drop(result);
    unsafe { scoped_test_failure(O::KIND, 0) };
    let retry = O::create(O::storage(2));
    check(
        retry.is_ok(),
        "completed failure cleanup permits initialization retry",
    );
    drop(retry);
}
fn forgotten_owner<O: Ops>() {
    let Some(owner) = required(O::create(O::storage(2))) else {
        return;
    };
    std::mem::forget(owner);
    let before = count(O::KIND, INIT);
    let second = O::create(O::storage(2));
    #[cfg(zero_heap)]
    {
        check(
            matches!(second, Err(Error::Busy)),
            "forgotten static owner retains admission",
        );
        check(
            count(O::KIND, INIT) == before,
            "forgotten claim stops native reinitialization",
        );
    }
    #[cfg(not(zero_heap))]
    {
        check(
            second.is_ok(),
            "forgotten heap owner leaves unused static storage independent",
        );
        check(
            count(O::KIND, INIT) == before + 1,
            "new heap owner uses fresh native backing",
        );
    }
    drop(second);
}
fn invalid_resource_context<O: Ops>(dropping: bool, isr: bool) {
    if dropping {
        let Some(owner) = required(O::create(O::storage(2))) else {
            return;
        };
        let before = count(O::KIND, DEINIT) + count(O::KIND, DESTROY);
        context(0, i32::from(isr));
        drop(owner);
        let after = count(O::KIND, DEINIT) + count(O::KIND, DESTROY);
        normal_context();
        check(
            before == after,
            "unsupported resource Drop avoids native entry",
        );
        check(
            live(O::KIND) == 1,
            "unsupported Drop retains native backing",
        );
        #[cfg(zero_heap)]
        {
            let before = count(O::KIND, INIT);
            let retry = O::create(O::storage(2));
            check(
                matches!(retry, Err(Error::Busy)),
                "retained static backing stays claimed",
            );
            check(
                count(O::KIND, INIT) == before,
                "retained claim stops native reinitialization",
            );
            drop(retry);
        }
    } else {
        let before = count(O::KIND, INIT) + count(O::KIND, CREATE);
        context(0, i32::from(isr));
        let result = O::create(O::storage(2));
        normal_context();
        check(
            matches!(result, Err(Error::NotSupported)),
            "unsupported construction reports context error",
        );
        check(
            before == count(O::KIND, INIT) + count(O::KIND, CREATE),
            "construction context check precedes native entry",
        );
        drop(result);
        let retry = O::create(O::storage(2));
        check(
            retry.is_ok(),
            "context rejection leaves static admission available",
        );
        drop(retry);
    }
}
fn raw_origin<O: Ops>() {
    #[cfg(zero_heap)]
    {
        let Some(typed) = required(O::create(O::storage(2))) else {
            return;
        };
        // SAFETY: independent complete raw static, exclusive and stable until cleanup.
        let raw = unsafe { O::raw() };
        check(raw.is_ok(), "exclusive independent raw origin initializes");
        drop(raw);
        let before = count(O::KIND, INIT);
        let duplicate = O::create(O::storage(2));
        check(
            matches!(duplicate, Err(Error::Busy)),
            "raw teardown cannot clear another typed claim",
        );
        check(
            before == count(O::KIND, INIT),
            "raw origin leaves typed admission intact",
        );
        drop(duplicate);
        drop(typed);
    }
    #[cfg(not(zero_heap))]
    duplicate::<O>();
}
fn macros<O: Ops>() {
    let before = count(O::KIND, NATIVE_DEINIT);
    let first = O::macro_owner();
    drop(first);
    let second = O::macro_owner();
    drop(second);
    check(
        count(O::KIND, NATIVE_DEINIT) == before + 2,
        "same macro site permits completed sequential reuse",
    );
}

#[cfg(scoped_parent)]
trait ParentRecursiveScopes {
    fn with_lock<T>(&self, f: impl FnOnce() -> ove::Result<T>) -> ove::Result<T>;
    fn try_with_lock<T>(&self, f: impl FnOnce() -> ove::Result<T>) -> ove::Result<T>;
    fn try_with_lock_for<T>(
        &self,
        d: Duration,
        f: impl FnOnce() -> ove::Result<T>,
    ) -> ove::Result<T>;
    #[cfg(has_time)]
    fn try_with_lock_until<T>(
        &self,
        d: ove::time::Instant,
        f: impl FnOnce() -> ove::Result<T>,
    ) -> ove::Result<T>;
}
#[cfg(scoped_parent)]
impl ParentRecursiveScopes for RecursiveMutex {
    fn with_lock<T>(&self, f: impl FnOnce() -> ove::Result<T>) -> ove::Result<T> {
        let _guard = self.lock()?;
        f()
    }
    fn try_with_lock<T>(&self, f: impl FnOnce() -> ove::Result<T>) -> ove::Result<T> {
        let _guard = self.try_lock()?;
        f()
    }
    fn try_with_lock_for<T>(
        &self,
        d: Duration,
        f: impl FnOnce() -> ove::Result<T>,
    ) -> ove::Result<T> {
        let _guard = self.try_lock_for(d)?;
        f()
    }
    #[cfg(has_time)]
    fn try_with_lock_until<T>(
        &self,
        d: ove::time::Instant,
        f: impl FnOnce() -> ove::Result<T>,
    ) -> ove::Result<T> {
        let _guard = self.try_lock_until(d)?;
        f()
    }
}

fn mutex_scope(case: &str) {
    let Some(mutex) = required(M::create(M::storage(2))) else {
        return;
    };
    match case {
        "forget" | "error" => {
            let result = mutex.with_lock(|access| {
                std::mem::forget(access);
                if case == "error" {
                    Err(Error::Io)
                } else {
                    Ok(())
                }
            });
            check(
                if case == "error" {
                    matches!(result, Err(Error::Io))
                } else {
                    result.is_ok()
                },
                "callback outcome propagates",
            );
            check(
                depth(MUTEX) == 0,
                "forgotten view leaves private release intact",
            );
        }
        "drop" => {
            let result = mutex.with_lock(|access| {
                drop(access);
                check(
                    depth(MUTEX) == 1,
                    "view Drop keeps the private acquisition until callback end",
                );
                Ok(())
            });
            check(
                result.is_ok() && depth(MUTEX) == 0,
                "callback end releases native acquisition",
            );
        }
        "reentry" => {
            // SAFETY: model only extra logical recursive levels over one real
            // initialized POSIX mutex. Neither admitted callback dereferences T.
            unsafe { scoped_test_recursive_model(true) };
            let mut callbacks = 0;
            let result = mutex.with_lock(|_access| {
                let second = mutex.try_with_lock(|_second| {
                    callbacks += 1;
                    Ok(())
                });
                check(
                    matches!(second, Err(Error::Busy)) && callbacks == 0,
                    "native recursion cannot issue a second data view",
                );
                check(
                    depth(MUTEX) == 1,
                    "reentry rejection balances only its temporary native level",
                );
                Ok(())
            });
            check(
                result.is_ok() && depth(MUTEX) == 0,
                "original private scope releases normally",
            );
            unsafe { scoped_test_recursive_model(false) };
        }
        "masked_release" => {
            let result = mutex.with_lock(|_access| {
                context(0, 0);
                Ok(())
            });
            check(
                result.is_ok() && depth(MUTEX) == 0,
                "private owned release survives callback mask changes",
            );
            normal_context();
        }
        "blocked_entry" | "startup_entry" => {
            let before = count(MUTEX, LOCK);
            context(if case == "startup_entry" { RESOURCE } else { 0 }, 0);
            let result = mutex.try_with_lock_for(Duration::ZERO, |_| Ok(()));
            normal_context();
            check(
                matches!(result, Err(Error::NotSupported)),
                "unsupported blocking context reports an error",
            );
            check(
                count(MUTEX, LOCK) == before,
                "blocking admission precedes native entry",
            );
        }
        "deadline_entry" => {
            #[cfg(has_time)]
            {
                let before = count(MUTEX, LOCK);
                let deadline = ove::time::Instant::now() + Duration::from_nanos(1000);
                let clock = unsafe { scoped_test_clock_calls() };
                context(0, 0);
                let result = mutex.try_with_lock_until(deadline, |_| Ok(()));
                normal_context();
                check(
                    matches!(result, Err(Error::NotSupported)),
                    "deadline acquisition rejects unsupported context",
                );
                check(
                    count(MUTEX, LOCK) == before && unsafe { scoped_test_clock_calls() } == clock,
                    "context check precedes deadline and native helpers",
                );
            }
            #[cfg(not(has_time))]
            check(false, "deadline case requires the optional clock profile");
        }
        _ => unreachable!(),
    }
    normal_context();
    retire_current();
    check(
        mutex.try_with_lock(|_| Ok(())).is_ok(),
        "restored mutex acquisition succeeds",
    );
    retire_current();
    drop(mutex);
}
fn recursive_scope(case: &str) {
    let Some(mutex) = required(R::create(R::storage(2))) else {
        return;
    };
    match case {
        "forget" => {
            // The old API exposes a release owner. The new no-data callback
            // never exposes its private lease; no shim cleanup is attributed to it.
            #[cfg(scoped_parent)]
            {
                if let Ok(guard) = mutex.lock() {
                    std::mem::forget(guard);
                } else {
                    check(false, "ordinary recursive acquisition succeeds");
                }
            }
            #[cfg(not(scoped_parent))]
            check(
                mutex.with_lock(|| Ok(())).is_ok(),
                "recursive callback completes",
            );
            check(
                depth(RECURSIVE) == 0,
                "recursive private ownership retires before worker return",
            );
        }
        "error" => {
            let result: ove::Result<()> = mutex.with_lock(|| Err(Error::Io));
            check(
                matches!(result, Err(Error::Io)) && depth(RECURSIVE) == 0,
                "recursive callback error retires its native level",
            );
        }
        "nested" => {
            let result = mutex.with_lock(|| {
                mutex.with_lock(|| {
                    check(
                        depth(RECURSIVE) == 2,
                        "nested recursive scopes hold matching levels",
                    );
                    Ok(())
                })
            });
            check(
                result.is_ok() && depth(RECURSIVE) == 0,
                "nested recursive scopes balance all levels",
            );
        }
        "masked_release" => {
            let result = mutex.with_lock(|| {
                context(0, 0);
                Ok(())
            });
            check(
                result.is_ok() && depth(RECURSIVE) == 0,
                "recursive owned release survives callback mask changes",
            );
            normal_context();
        }
        "blocked_entry" | "startup_entry" => {
            let before = count(RECURSIVE, LOCK);
            context(if case == "startup_entry" { RESOURCE } else { 0 }, 0);
            let result = mutex.try_with_lock_for(Duration::ZERO, || Ok(()));
            normal_context();
            check(
                matches!(result, Err(Error::NotSupported)),
                "recursive blocking context rejection propagates",
            );
            check(
                count(RECURSIVE, LOCK) == before,
                "recursive admission precedes native entry",
            );
        }
        "deadline_entry" => {
            #[cfg(has_time)]
            {
                let before = count(RECURSIVE, LOCK);
                let deadline = ove::time::Instant::now() + Duration::from_nanos(1000);
                let clock = unsafe { scoped_test_clock_calls() };
                context(0, 0);
                let result = mutex.try_with_lock_until(deadline, || Ok(()));
                normal_context();
                check(
                    matches!(result, Err(Error::NotSupported)),
                    "recursive deadline context rejection propagates",
                );
                check(
                    count(RECURSIVE, LOCK) == before
                        && unsafe { scoped_test_clock_calls() } == clock,
                    "recursive context precedes native deadline helpers",
                );
            }
            #[cfg(not(has_time))]
            check(false, "recursive deadline case requires clock profile");
        }
        _ => unreachable!(),
    }
    normal_context();
    retire_current();
    check(
        mutex.try_with_lock(|| Ok(())).is_ok(),
        "restored recursive acquisition succeeds",
    );
    retire_current();
    drop(mutex);
}
fn cv_error(mode: u32, blocked: bool, propagate: bool) {
    let Some(mutex) = required(M::create(M::storage(2))) else {
        return;
    };
    let Some(cv) = required(C::create(C::storage(2))) else {
        drop(mutex);
        return;
    };
    // SAFETY: closed modeled error modes initialize truthful held output; actual
    // independent same-owner release is performed only by the unheld model.
    unsafe { scoped_test_wait_model(mode) };
    let before = count(CONDVAR, WAIT);
    let mut entered = false;
    let result = mutex.with_lock(|access| {
        entered = true;
        if blocked {
            context(0, 0);
        }
        let result = cv.wait_for(access, Duration::ZERO);
        let valid = if blocked {
            matches!(result, Err(Error::NotSupported))
        } else {
            matches!(result, Err(Error::Io))
        };
        check(valid, "condition wait reports its explicit error");
        if propagate {
            result.map(|_| ())
        } else {
            Ok(())
        }
    });
    normal_context();
    check(
        entered && count(CONDVAR, WAIT) == before + u32::from(!blocked),
        "modeled wait callback and expected native entry occurred",
    );
    check(
        if propagate {
            result.is_err()
        } else {
            result.is_ok()
        },
        "caught or propagated wait outcome is preserved",
    );
    check(
        depth(MUTEX) == 0,
        "condition wait error leaves no private native debt",
    );
    if blocked {
        check(
            count(CONDVAR, WAIT) == before,
            "wait admission precedes native release",
        );
    }
    retire_current();
    unsafe { scoped_test_wait_model(0) };
    let control = mutex.with_lock(|access| {
        let (_access, outcome) = cv.wait_for(access, Duration::ZERO)?;
        check(
            outcome.timed_out(),
            "restored condition wait reaches real timeout",
        );
        Ok(())
    });
    check(
        control.is_ok(),
        "restored condition acquisition and wait succeed",
    );
    retire_current();
    drop(cv);
    drop(mutex);
}
fn cv_timeout() {
    let Some(mutex) = required(M::create(M::storage(2))) else {
        return;
    };
    let Some(cv) = required(C::create(C::storage(2))) else {
        drop(mutex);
        return;
    };
    let result = mutex.with_lock(|access| {
        let (access, outcome) = cv.wait_for(access, Duration::ZERO)?;
        check(
            outcome.timed_out() && depth(MUTEX) == 1,
            "real POSIX timeout reacquires exactly once",
        );
        std::mem::forget(access);
        Ok(())
    });
    check(
        result.is_ok() && depth(MUTEX) == 0,
        "forgotten reacquired view retains private cleanup",
    );
    retire_current();
    drop(cv);
    drop(mutex);
}
fn cv_signal_context() {
    let Some(cv) = required(C::create(C::storage(2))) else {
        return;
    };
    let before = count(CONDVAR, NOTIFY);
    context(0, 0);
    cv.signal();
    cv.broadcast();
    normal_context();
    check(
        count(CONDVAR, NOTIFY) == before,
        "unsupported condition signaling has no native effect",
    );
    cv.signal();
    cv.broadcast();
    check(
        count(CONDVAR, NOTIFY) == before + 2,
        "restored signaling reaches native provider",
    );
    drop(cv);
}

fn genuine_contender() {
    let Some(mutex) = required(M::create(M::storage(2))) else {
        return;
    };
    let ready = core::sync::atomic::AtomicBool::new(false);
    let release = core::sync::atomic::AtomicBool::new(false);
    let done = core::sync::atomic::AtomicBool::new(false);
    let in_holder = core::sync::atomic::AtomicBool::new(false);
    std::thread::scope(|scope| {
        let holder = std::thread::Builder::new()
            .stack_size(64 * 1024 * 1024)
            .spawn_scoped(scope, || {
                let _exit = WorkerExit;
                mutex.with_lock(|_access| {
                    in_holder.store(true, Ordering::Release);
                    ready.store(true, Ordering::Release);
                    while !release.load(Ordering::Acquire) {
                        std::thread::yield_now();
                    }
                    in_holder.store(false, Ordering::Release);
                    Ok(())
                })
            })
            .expect("bounded holder");
        while !ready.load(Ordering::Acquire) {
            std::thread::yield_now();
        }
        let before = count(MUTEX, LOCK);
        let contender = std::thread::Builder::new()
            .stack_size(64 * 1024 * 1024)
            .spawn_scoped(scope, || {
                let _exit = WorkerExit;
                let result = mutex.try_with_lock_for(Duration::from_secs(1), |_access| {
                    check(
                        !in_holder.load(Ordering::Acquire),
                        "native exclusion outlasts holder callback",
                    );
                    Ok(())
                });
                done.store(true, Ordering::Release);
                result
            })
            .expect("bounded contender");
        while count(MUTEX, LOCK) == before && !done.load(Ordering::Acquire) {
            std::thread::yield_now();
        }
        check(
            count(MUTEX, LOCK) == before + 1,
            "other-thread contender reaches native acquisition while issued",
        );
        release.store(true, Ordering::Release);
        check(
            matches!(holder.join(), Ok(Ok(()))),
            "holder scope completed",
        );
        check(
            matches!(contender.join(), Ok(Ok(()))),
            "genuine contender acquires after release",
        );
    });
    check(depth(MUTEX) == 0, "joined contenders leave no native debt");
    drop(mutex);
}
fn cv_second_owner() {
    let Some(mutex) = required(M::create(M::storage(2))) else {
        return;
    };
    let Some(cv) = required(C::create(C::storage(2))) else {
        drop(mutex);
        return;
    };
    let ready = core::sync::atomic::AtomicBool::new(false);
    let old_done = core::sync::atomic::AtomicBool::new(false);
    unsafe {
        scoped_test_recursive_model(true);
        scoped_test_wait_model(2);
    }
    gate(CONDVAR, WAIT_RETURN_STAGE, true);
    std::thread::scope(|scope| {
        let old = std::thread::Builder::new()
            .stack_size(64 * 1024 * 1024)
            .spawn_scoped(scope, || {
                let _exit = WorkerExit;
                let result =
                    mutex.with_lock(|access| cv.wait_for(access, Duration::ZERO).map(|_| ()));
                check(
                    matches!(result, Err(Error::Io)),
                    "unheld old wait reports its error",
                );
                old_done.store(true, Ordering::Release);
            })
            .expect("bounded old waiter");
        wait_entered(CONDVAR, WAIT_RETURN_STAGE);
        let next = std::thread::Builder::new()
            .stack_size(64 * 1024 * 1024)
            .spawn_scoped(scope, || {
                let _exit = WorkerExit;
                mutex.with_lock(|_access| {
                    ready.store(true, Ordering::Release);
                    while !old_done.load(Ordering::Acquire) {
                        std::thread::yield_now();
                    }
                    let mut admitted = false;
                    let result = mutex.try_with_lock(|_second| {
                        admitted = true;
                        Ok(())
                    });
                    check(
                        matches!(result, Err(Error::Busy)) && !admitted,
                        "unheld old lease cannot clear a later owner's loan",
                    );
                    Ok(())
                })
            })
            .expect("bounded next owner");
        while !ready.load(Ordering::Acquire) {
            std::thread::yield_now();
        }
        gate(CONDVAR, WAIT_RETURN_STAGE, false);
        check(old.join().is_ok(), "old unheld worker returned normally");
        check(
            matches!(next.join(), Ok(Ok(()))),
            "later native owner completed normally",
        );
    });
    unsafe {
        scoped_test_recursive_model(false);
        scoped_test_wait_model(0);
    }
    check(
        depth(MUTEX) == 0,
        "both condition owners retired their native levels",
    );
    retire_current();
    drop(cv);
    drop(mutex);
}
fn cv_real_wake() {
    let Some(mutex) = required(M::create(M::storage(2))) else {
        return;
    };
    let Some(cv) = required(C::create(C::storage(2))) else {
        drop(mutex);
        return;
    };
    let signaled = core::sync::atomic::AtomicBool::new(false);
    std::thread::scope(|scope| {
        let waiter = std::thread::Builder::new()
            .stack_size(64 * 1024 * 1024)
            .spawn_scoped(scope, || {
                let _exit = WorkerExit;
                mutex.with_lock(|access| {
                    let _access = cv.wait_while(access, |_| !signaled.load(Ordering::Acquire))?;
                    check(
                        depth(MUTEX) == 1,
                        "real POSIX wake reacquires one native level",
                    );
                    Ok(())
                })
            })
            .expect("bounded genuine waiter");
        // The observer announces registration before actual atomic native release.
        // Acquiring the same native mutex prevents signal preceding that release.
        unsafe { scoped_test_wait_registered() };
        let signal = mutex.with_lock(|_access| {
            signaled.store(true, Ordering::Release);
            cv.signal();
            Ok(())
        });
        check(
            signal.is_ok(),
            "genuine signaller acquired the released native mutex",
        );
        check(
            matches!(waiter.join(), Ok(Ok(()))),
            "genuine condition waiter completed",
        );
    });
    check(
        depth(MUTEX) == 0,
        "genuine condition scopes release before join",
    );
    drop(cv);
    drop(mutex);
}
fn native_context_flags() {
    let normal = unsafe { ove::ffi::ove_sync_context() };
    let depth_before = unsafe { scoped_test_actual_critical_depth() };
    check(
        normal == (RESOURCE | BLOCKING),
        "ordinary POSIX resource and blocking flags are available",
    );
    unsafe { posix_irq_enter_isr() };
    let isr = unsafe { ove::ffi::ove_sync_context() };
    unsafe { posix_irq_leave_isr() };
    check(
        isr == 0,
        "actual POSIX dispatcher metadata rejects ISR context",
    );
    check(
        unsafe { scoped_test_actual_critical_depth() } == depth_before,
        "context query preserves actual critical depth",
    );
    #[cfg(has_async)]
    {
        let key = unsafe { ove::ffi::ove_irq_lock() };
        let held_before = unsafe { scoped_test_actual_critical_depth() };
        let masked = unsafe { ove::ffi::ove_sync_context() };
        let held_after = unsafe { scoped_test_actual_critical_depth() };
        unsafe { ove::ffi::ove_irq_unlock(key) };
        check(
            masked == 0 && held_before == depth_before + 1 && held_before == held_after,
            "readonly actual critical-section context query",
        );
    }
    check(
        unsafe { ove::ffi::ove_sync_context() } == normal,
        "ordinary context flags restore after controls",
    );
}
fn actual_masked_release() {
    #[cfg(has_async)]
    {
        let Some(mutex) = required(M::create(M::storage(2))) else {
            return;
        };
        let key = core::cell::Cell::new(None);
        struct Restore<'a>(&'a core::cell::Cell<Option<ove::ffi::ove_irq_key_t>>);
        impl Drop for Restore<'_> {
            fn drop(&mut self) {
                if let Some(key) = self.0.take() {
                    unsafe { ove::ffi::ove_irq_unlock(key) };
                }
            }
        }
        let restore = Restore(&key);
        let result = mutex.with_lock(|access| {
            key.set(Some(unsafe { ove::ffi::ove_irq_lock() }));
            std::mem::forget(access);
            Ok(())
        });
        let observed = depth(MUTEX);
        let flags = unsafe { ove::ffi::ove_sync_context() };
        let critical = unsafe { scoped_test_actual_critical_depth() };
        drop(restore);
        check(
            result.is_ok() && observed == 0,
            "private release completes before actual IRQ restoration",
        );
        check(
            flags == 0 && critical == 1,
            "actual masked context was retained through private cleanup",
        );
        retire_current();
        drop(mutex);
    }
    #[cfg(not(has_async))]
    check(false, "actual mask case requires critical-section profile");
}
static VALUE_DROPS: AtomicU32 = AtomicU32::new(0);
struct Value;
impl Drop for Value {
    fn drop(&mut self) {
        VALUE_DROPS.fetch_add(1, Ordering::Relaxed);
    }
}
fn value_lifecycle(case: &str) {
    match case {
        "busy" => {
            let Some(owner) = required(Mutex::create(M::storage(2), Value)) else {
                return;
            };
            let second = Mutex::create(M::storage(2), Value);
            #[cfg(zero_heap)]
            check(
                matches!(second, Err(Error::Busy)),
                "value construction reports live static claim",
            );
            #[cfg(not(zero_heap))]
            check(second.is_ok(), "heap value constructors stay independent");
            drop(second);
            check(
                VALUE_DROPS.load(Ordering::Relaxed) == 1,
                "rejected or dropped second value drops once",
            );
            drop(owner);
            check(
                VALUE_DROPS.load(Ordering::Relaxed) == 2,
                "original value drops exactly once",
            );
        }
        "error" => {
            unsafe { scoped_test_failure(MUTEX, 2) };
            let owner = Mutex::create(M::storage(2), Value);
            check(
                matches!(owner, Err(Error::NoMemory)),
                "value constructor propagates cleaned error",
            );
            drop(owner);
            unsafe { scoped_test_failure(MUTEX, 0) };
            check(
                VALUE_DROPS.load(Ordering::Relaxed) == 1,
                "failed initialization drops its value once",
            );
        }
        "retained_drop" => {
            let Some(owner) = required(Mutex::create(M::storage(2), Value)) else {
                return;
            };
            context(0, 1);
            drop(owner);
            normal_context();
            check(
                VALUE_DROPS.load(Ordering::Relaxed) == 1 && live(MUTEX) == 1,
                "native retention still drops the value once",
            );
        }
        "into_inner" | "retained_into_inner" => {
            #[cfg(not(zero_heap))]
            {
                let Some(owner) = required(Mutex::create(M::storage(2), Value)) else {
                    return;
                };
                let before = count(MUTEX, DESTROY);
                if case == "retained_into_inner" {
                    context(0, 1);
                }
                let value = owner.into_inner();
                normal_context();
                check(
                    value.is_ok() && VALUE_DROPS.load(Ordering::Relaxed) == 0,
                    "into_inner transfers without dropping its value",
                );
                if case == "retained_into_inner" {
                    check(
                        count(MUTEX, DESTROY) == before && live(MUTEX) == 1,
                        "into_inner retains unavailable native cleanup",
                    );
                }
                drop(value);
                check(
                    VALUE_DROPS.load(Ordering::Relaxed) == 1,
                    "extracted value drops once",
                );
            }
            #[cfg(zero_heap)]
            check(false, "into_inner case requires heap mode");
        }
        _ => unreachable!(),
    }
}

fn cv_deadline_context(predicate: bool) {
    #[cfg(has_time)]
    {
        let Some(mutex) = required(M::create(M::storage(2))) else {
            return;
        };
        let Some(cv) = required(C::create(C::storage(2))) else {
            drop(mutex);
            return;
        };
        let before = count(CONDVAR, WAIT);
        let deadline = ove::time::Instant::now() + Duration::from_nanos(1000);
        let clock = unsafe { scoped_test_clock_calls() };
        let mut predicates = 0;
        let result = mutex.with_lock(|access| {
            context(0, 0);
            let wait = if predicate {
                cv.wait_while_for(access, Duration::from_nanos(100), |_| {
                    predicates += 1;
                    false
                })
            } else {
                cv.wait_until(access, deadline)
            };
            check(
                matches!(wait, Err(Error::NotSupported)),
                "condition deadline rejects unsupported context",
            );
            Ok(())
        });
        normal_context();
        check(
            result.is_ok(),
            "handled condition deadline error returns normally",
        );
        check(
            count(CONDVAR, WAIT) == before
                && unsafe { scoped_test_clock_calls() } == clock
                && predicates == 0,
            "context admission precedes condition clocks and predicates",
        );
        check(
            depth(MUTEX) == 0,
            "condition deadline rejection keeps private cleanup",
        );
        retire_current();
        let control = mutex.with_lock(|access| {
            let (_access, outcome) = cv.wait_for(access, Duration::ZERO)?;
            check(
                outcome.timed_out(),
                "restored deadline control reaches real timeout",
            );
            Ok(())
        });
        check(
            control.is_ok(),
            "restored deadline acquisition and wait succeed",
        );
        retire_current();
        drop(cv);
        drop(mutex);
    }
    #[cfg(not(has_time))]
    {
        let _ = predicate;
        check(false, "condition deadline case requires clock profile");
    }
}

const LIFECYCLE: &[&str] = &[
    "controls",
    "canary",
    "live",
    "init_overlap",
    "drop_overlap",
    "failure",
    "cleaned_error",
    "forget_owner",
    "construct_isr",
    "construct_masked",
    "drop_isr",
    "drop_masked",
    "raw",
    "macros",
];
fn lifecycle<O: Ops>(case: &str) {
    match case {
        "controls" | "canary" => (),
        "live" => duplicate::<O>(),
        "init_overlap" => overlap::<O>(INIT_STAGE),
        "drop_overlap" => overlap::<O>(DEINIT_STAGE),
        "failure" => failed_init::<O>(1),
        "cleaned_error" => failed_init::<O>(2),
        "forget_owner" => forgotten_owner::<O>(),
        "construct_isr" => invalid_resource_context::<O>(false, true),
        "construct_masked" => invalid_resource_context::<O>(false, false),
        "drop_isr" => invalid_resource_context::<O>(true, true),
        "drop_masked" => invalid_resource_context::<O>(true, false),
        "raw" => raw_origin::<O>(),
        "macros" => macros::<O>(),
        _ => unreachable!(),
    }
}
fn run_family<O: Ops>(case: &str, action: impl FnOnce()) {
    if case == "canary" {
        check(false, "explicit early failure canary");
    }
    controls::<O>();
    action();
    normal_context();
    controls::<O>();
}
fn total(index: u32) -> u32 {
    (0..3).map(|kind| count(kind, index)).sum()
}
fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 2 {
        std::process::exit(2);
    }
    let name = &args[1];
    let exit_guard = WorkerExit;
    if let Some(case) = name
        .strip_prefix("mutex_")
        .filter(|case| LIFECYCLE.contains(case))
    {
        run_family::<M>(case, || lifecycle::<M>(case));
    } else if let Some(case) = name
        .strip_prefix("recursive_")
        .filter(|case| LIFECYCLE.contains(case))
    {
        run_family::<R>(case, || lifecycle::<R>(case));
    } else if let Some(case) = name
        .strip_prefix("condvar_")
        .filter(|case| LIFECYCLE.contains(case))
    {
        run_family::<C>(case, || lifecycle::<C>(case));
    } else if let Some(case) = name.strip_prefix("scope_mutex_").filter(|case| {
        [
            "forget",
            "error",
            "drop",
            "reentry",
            "masked_release",
            "blocked_entry",
            "startup_entry",
            "deadline_entry",
        ]
        .contains(case)
    }) {
        run_family::<M>(case, || mutex_scope(case));
    } else if let Some(case) = name.strip_prefix("scope_recursive_").filter(|case| {
        [
            "forget",
            "error",
            "nested",
            "masked_release",
            "blocked_entry",
            "startup_entry",
            "deadline_entry",
        ]
        .contains(case)
    }) {
        run_family::<R>(case, || recursive_scope(case));
    } else {
        match name.as_str() {
            "native_flags" => run_family::<M>("native_flags", native_context_flags),
            "actual_masked_release" => {
                run_family::<M>("actual_masked_release", actual_masked_release)
            }
            "genuine_contender" => run_family::<M>("genuine_contender", genuine_contender),
            "cv_second_owner" => run_family::<C>("second_owner", cv_second_owner),
            "cv_real_wake" => run_family::<C>("real_wake", cv_real_wake),
            "value_busy" => run_family::<M>("value_busy", || value_lifecycle("busy")),
            "value_error" => run_family::<M>("value_error", || value_lifecycle("error")),
            "value_retained_drop" => {
                run_family::<M>("value_retained_drop", || value_lifecycle("retained_drop"))
            }
            "value_into_inner" => {
                run_family::<M>("value_into_inner", || value_lifecycle("into_inner"))
            }
            "value_retained_into_inner" => run_family::<M>("value_retained_into_inner", || {
                value_lifecycle("retained_into_inner")
            }),
            "cv_deadline_context" => {
                run_family::<C>("cv_deadline_context", || cv_deadline_context(false))
            }
            "cv_predicate_context" => {
                run_family::<C>("cv_predicate_context", || cv_deadline_context(true))
            }
            "cv_held_error" => run_family::<C>("held_error", || cv_error(1, false, false)),
            "cv_unheld_error" => run_family::<C>("unheld_error", || cv_error(2, false, false)),
            "cv_propagated_held_error" => {
                run_family::<C>("propagated_held_error", || cv_error(1, false, true))
            }
            "cv_blocked_wait" => run_family::<C>("blocked_wait", || cv_error(1, true, false)),
            "cv_timeout_forget" => run_family::<C>("timeout_forget", cv_timeout),
            "cv_signal_context" => run_family::<C>("signal_context", cv_signal_context),
            _ => std::process::exit(2),
        }
    }
    drop(exit_guard);
    normal_context();
    let retained: u32 = (0..3).map(live).sum();
    let expected_retained = u32::from(
        name.ends_with("_forget_owner")
            || name.ends_with("_drop_isr")
            || name.ends_with("_drop_masked")
            || name == "value_retained_drop"
            || name == "value_retained_into_inner",
    );
    check(total(FORBIDDEN) == 0, "no forbidden native entry occurred");
    check(
        total(RETIRE) == 0,
        "no independent acquisition retirement was needed",
    );
    check(
        retained == expected_retained,
        "only intentional backing is retained",
    );
    check(
        total(NATIVE_INIT) == total(NATIVE_DEINIT) + retained,
        "native lifetime balance agrees before harness cleanup",
    );
    // SAFETY: all case owners are gone, scoped workers joined and current-owned
    // debt retired while live. Retained backing is counted before cleanup.
    let cleanup = unsafe { scoped_test_cleanup_retained() };
    check(
        unsafe { scoped_test_registry_ok() },
        "final registry pointers and generations are valid",
    );
    println!(
        "{name}: failures={} checks={} init={} deinit={} locks={} unlocks={} duplicates={} forbidden={} retirements={} retained={} harness_cleanup={}",
        FAILURES.load(Ordering::Relaxed),
        CHECKS.load(Ordering::Relaxed),
        total(NATIVE_INIT),
        total(NATIVE_DEINIT),
        total(LOCK),
        total(UNLOCK),
        total(DUPLICATE),
        total(FORBIDDEN),
        total(RETIRE),
        retained,
        cleanup
    );
    std::process::exit(i32::from(FAILURES.load(Ordering::Relaxed) != 0));
}
