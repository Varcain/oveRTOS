// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

//! Operation state shared by the owned async SPI and I2C controllers.
//!
//! One stable header holds the lifetime count, a cross-core metadata lock,
//! the internal TX/RX staging and the bus's native controller. Futures never
//! hold a count across Pending; native completion owns its own count.

use core::cell::UnsafeCell;
use core::ffi::{c_int, c_void};
use core::ops::{Deref, DerefMut};
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering, fence};
use core::task::Waker;

use crate::bindings;
use crate::error::{Error, Result};

/// The bus-specific part of a header.
pub(super) trait Bus {
    /// Bus name used in panic messages.
    const NAME: &'static str;
    /// Native controller storage and handle, kept in the stable header.
    type Native;
    /// Deinitialize the native controller if its initialization succeeded.
    unsafe fn deinit(native: &Self::Native);
    /// Allocate stable backing through the bus's native allocator.
    #[cfg(not(zero_heap))]
    unsafe fn alloc(size: usize, alignment: usize) -> *mut c_void;
    /// Release backing returned by [`Bus::alloc`].
    #[cfg(not(zero_heap))]
    unsafe fn free(pointer: *mut c_void);
}

/// Internal TX/RX staging. Capacities count bus words (bytes for I2C).
#[derive(Copy, Clone)]
pub(super) struct Staging {
    pub tx: *mut u8,
    pub rx: *mut u8,
    pub tx_capacity: usize,
    pub rx_capacity: usize,
    pub allocation: *mut c_void,
}

impl Staging {
    #[cfg(not(zero_heap))]
    pub const EMPTY: Self = Self {
        tx: ptr::null_mut(),
        rx: ptr::null_mut(),
        tx_capacity: 0,
        rx_capacity: 0,
        allocation: ptr::null_mut(),
    };
}

#[derive(Copy, Clone, Eq, PartialEq)]
pub(super) enum Phase {
    Idle,
    Preparing,
    Active,
    Ready,
    Consuming,
}

struct Metadata {
    phase: Phase,
    generation: usize,
    result: i32,
    closing: bool,
    waiter: Option<Waker>,
    staging: Staging,
}

pub(super) struct Header<B: Bus> {
    refs: AtomicUsize,
    locked: AtomicBool,
    claim: *const AtomicBool,
    metadata: UnsafeCell<Metadata>,
    pub native: B::Native,
}

/// A synchronous call or native/callback lifetime, never stored in a Future.
pub(super) struct Lease<B: Bus> {
    header: NonNull<Header<B>>,
}

impl<B: Bus> Lease<B> {
    pub unsafe fn acquire(header: NonNull<Header<B>>) -> Self {
        // SAFETY: the caller already owns a live reference. A checked CAS
        // excludes overflow rather than incrementing and checking afterward.
        unsafe {
            (*header.as_ptr())
                .refs
                .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
                    n.checked_add(1)
                        .filter(|next| n != 0 && *next <= isize::MAX as usize)
                })
                .unwrap_or_else(|n| panic!("{} lifetime count overflow: {n}", B::NAME));
        }
        Self { header }
    }

    unsafe fn adopt(header: NonNull<Header<B>>) -> Self {
        Self { header }
    }
}

impl<B: Bus> Drop for Lease<B> {
    fn drop(&mut self) {
        // SAFETY: this local owns exactly one count and never accesses the
        // header after releasing it. Final cleanup may reclaim its allocation.
        unsafe { release(self.header) };
    }
}

struct Locked<B: Bus> {
    header: NonNull<Header<B>>,
    irq: bindings::ove_irq_key_t,
}

impl<B: Bus> Locked<B> {
    unsafe fn acquire(header: NonNull<Header<B>>) -> Self {
        // Local masking prevents a same-core callback preempting the holder;
        // the atomic lock additionally serializes native callbacks on other cores.
        let irq = unsafe { bindings::ove_irq_lock() };
        unsafe {
            while (*header.as_ptr())
                .locked
                .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
            {
                core::hint::spin_loop();
            }
        }
        Self { header, irq }
    }
}

impl<B: Bus> Deref for Locked<B> {
    type Target = Metadata;
    fn deref(&self) -> &Metadata {
        // SAFETY: the lock serializes this interior-mutable metadata only.
        unsafe { &*(*self.header.as_ptr()).metadata.get() }
    }
}
impl<B: Bus> DerefMut for Locked<B> {
    fn deref_mut(&mut self) -> &mut Metadata {
        // SAFETY: this guard has exclusive metadata access until it unlocks.
        unsafe { &mut *(*self.header.as_ptr()).metadata.get() }
    }
}
impl<B: Bus> Drop for Locked<B> {
    fn drop(&mut self) {
        unsafe {
            (*self.header.as_ptr())
                .locked
                .store(false, Ordering::Release);
            bindings::ove_irq_unlock(self.irq);
        }
    }
}

/// Write the common header fields in place, holding the owner's count.
/// The caller then initializes `native` and calls [`release`] on failure.
pub(super) unsafe fn initialize<B: Bus>(
    header: NonNull<Header<B>>,
    claim: *const AtomicBool,
    staging: Staging,
) {
    // SAFETY: allocation/claim gives exclusive uninitialized backing. Write
    // fixed metadata in place; never create a payload-sized stack temporary.
    unsafe {
        let raw = header.as_ptr();
        ptr::addr_of_mut!((*raw).refs).write(AtomicUsize::new(1));
        ptr::addr_of_mut!((*raw).locked).write(AtomicBool::new(false));
        ptr::addr_of_mut!((*raw).claim).write(claim);
        ptr::addr_of_mut!((*raw).metadata).write(UnsafeCell::new(Metadata {
            phase: Phase::Idle,
            generation: 0,
            result: 0,
            closing: false,
            waiter: None,
            staging,
        }));
    }
}

pub(super) unsafe fn release<B: Bus>(header: NonNull<Header<B>>) {
    unsafe {
        let raw = header.as_ptr();
        if (*raw).refs.fetch_sub(1, Ordering::Release) != 1 {
            return;
        }
        fence(Ordering::Acquire);
        // The unique last-reference winner owns cleanup through native close
        // returning. No callback, poll or submit call can still use the header.
        let claim = (*raw).claim;
        let metadata = &mut *(*raw).metadata.get();
        let waiter = metadata.waiter.take();
        let allocation = metadata.staging.allocation;
        drop(waiter);
        B::deinit(&(*raw).native);
        #[cfg(not(zero_heap))]
        if !allocation.is_null() {
            B::free(allocation);
        }
        #[cfg(zero_heap)]
        let _ = allocation;
        ptr::drop_in_place(raw);
        if claim.is_null() {
            #[cfg(not(zero_heap))]
            B::free(raw.cast());
        } else {
            // This is the final storage access; another initializer may now
            // overwrite the header. No destructor/lock guard survives it.
            (*claim).store(false, Ordering::Release);
        }
    }
}

pub(super) unsafe fn close<B: Bus>(header: NonNull<Header<B>>) {
    let waiter = unsafe {
        let mut metadata = Locked::acquire(header);
        metadata.closing = true;
        metadata.waiter.take()
    };
    // Arbitrary Waker destruction runs outside the lock, with the owner count
    // still live. Native close occurs only after the last remaining lease.
    drop(waiter);
    unsafe { release(header) };
}

pub(super) unsafe fn detach<B: Bus>(header: NonNull<Header<B>>, generation: usize) {
    let _call = unsafe { Lease::acquire(header) };
    let waiter = unsafe {
        let mut metadata = Locked::acquire(header);
        if metadata.generation == generation {
            metadata.waiter.take()
        } else {
            None
        }
    };
    drop(waiter);
}

unsafe extern "C" fn completed<B: Bus>(result: c_int, user: *mut c_void) {
    // SAFETY: accepted submission transfers one context count to this callback.
    // It becomes a wake-tail count; no Future Drop is needed to release it.
    let header = unsafe { NonNull::new_unchecked(user.cast::<Header<B>>()) };
    let _callback = unsafe { Lease::adopt(header) };
    let waiter = unsafe {
        let mut metadata = Locked::acquire(header);
        metadata.result = result;
        metadata.phase = Phase::Ready;
        metadata.waiter.take()
    };
    // Final operation/staging/native access is above. A reentrant wake may
    // consume Ready, grow staging or drop the owner; only the independent
    // context count is touched afterward by this local callback lease.
    if let Some(waiter) = waiter {
        waiter.wake();
    }
}

/// Refuse new work while closing or busy (Idle and Ready are accepted), or
/// beyond static capacity.
/// Returns the current phase and staging for [`prepare`].
pub(super) unsafe fn check_idle<B: Bus>(
    header: NonNull<Header<B>>,
    tx_len: usize,
    rx_len: usize,
) -> Result<(Phase, Staging)> {
    unsafe {
        let metadata = Locked::acquire(header);
        if metadata.closing
            || matches!(
                metadata.phase,
                Phase::Active | Phase::Preparing | Phase::Consuming
            )
        {
            return Err(Error::BusBusy);
        }
        if !(*header.as_ptr()).claim.is_null()
            && (tx_len > metadata.staging.tx_capacity || rx_len > metadata.staging.rx_capacity)
        {
            return Err(Error::NoSpace);
        }
        Ok((metadata.phase, metadata.staging))
    }
}

/// Enter Preparing and grow staging to `tx_len`/`rx_len` words of `W`,
/// restoring `previous` if growth fails. On success the caller must reach
/// [`submit`] without returning early; nothing else leaves Preparing.
pub(super) unsafe fn prepare<B: Bus, W>(
    header: NonNull<Header<B>>,
    previous: Phase,
    old: Staging,
    tx_len: usize,
    rx_len: usize,
) -> Result<Staging> {
    unsafe {
        Locked::acquire(header).phase = Phase::Preparing;
        let stage = grow::<B, W>(header, old, tx_len, rx_len);
        if stage.is_err() {
            Locked::acquire(header).phase = previous;
        }
        stage
    }
}

unsafe fn grow<B: Bus, W>(
    header: NonNull<Header<B>>,
    old: Staging,
    tx_len: usize,
    rx_len: usize,
) -> Result<Staging> {
    if tx_len <= old.tx_capacity && rx_len <= old.rx_capacity {
        return Ok(old);
    }
    #[cfg(zero_heap)]
    {
        let _ = header;
        Err(Error::NoSpace)
    }
    #[cfg(not(zero_heap))]
    unsafe {
        if !(*header.as_ptr()).claim.is_null() {
            return Err(Error::NoSpace);
        }
        let tx_capacity = tx_len.max(old.tx_capacity);
        let rx_capacity = rx_len.max(old.rx_capacity);
        let count = tx_capacity
            .checked_add(rx_capacity)
            .ok_or(Error::InvalidParam)?;
        let layout = core::alloc::Layout::array::<W>(count).map_err(|_| Error::InvalidParam)?;
        let allocation = B::alloc(layout.size(), layout.align());
        if allocation.is_null() {
            return Err(Error::NoMemory);
        }
        let stage = Staging {
            tx: allocation.cast(),
            rx: allocation.cast::<W>().add(tx_capacity).cast(),
            tx_capacity,
            rx_capacity,
            allocation,
        };
        // No native operation retains old staging: Ready precedes this path,
        // and every old callback uses only its context lease after publication.
        if !old.allocation.is_null() {
            B::free(old.allocation);
        }
        Ok(stage)
    }
}

/// Publish `stage`, install `waiter` and submit one native operation.
/// `start` receives the completion callback and its context and returns
/// the native submission result. Returns the new operation's generation.
pub(super) unsafe fn submit<B: Bus>(
    header: NonNull<Header<B>>,
    stage: Staging,
    waiter: Waker,
    start: impl FnOnce(bindings::ove_dma_complete_cb, *mut c_void) -> c_int,
) -> Result<usize> {
    unsafe {
        let (generation, old_waiter) = {
            let mut metadata = Locked::acquire(header);
            metadata.staging = stage;
            metadata.generation = metadata.generation.wrapping_add(1);
            let old_waiter = metadata.waiter.replace(waiter);
            metadata.phase = Phase::Active;
            (metadata.generation, old_waiter)
        };
        drop(old_waiter);
        let submit = Lease::acquire(header);
        // The operation count is deliberately not stored in the Future.
        // Accepted callback adopts it; rejection below releases it explicitly.
        let operation = Lease::acquire(header);
        core::mem::forget(operation);
        let rc = start(Some(completed::<B>), header.as_ptr().cast());
        if let Err(error) = Error::from_code(rc) {
            let waiter = {
                let mut metadata = Locked::acquire(header);
                metadata.phase = Phase::Idle;
                metadata.waiter.take()
            };
            drop(Lease::adopt(header));
            drop(waiter);
            drop(submit);
            return Err(error);
        }
        // Callback/reentrant operations may already have reused metadata.
        // Retire only this submit call's independent count, with no state write.
        drop(submit);
        Ok(generation)
    }
}

pub(super) enum Outcome {
    Pending {
        generation: usize,
        replaced: Option<Waker>,
    },
    Ready {
        generation: usize,
        result: Result<()>,
        rx: *const u8,
        unused: Option<Waker>,
    },
    Idle {
        unused: Option<Waker>,
    },
}

pub(super) unsafe fn observe<B: Bus>(
    header: NonNull<Header<B>>,
    generation: Option<usize>,
    waiter: Option<Waker>,
) -> Outcome {
    unsafe {
        let mut metadata = Locked::acquire(header);
        if metadata.phase == Phase::Idle {
            return Outcome::Idle { unused: waiter };
        }
        if let Some(generation) = generation {
            assert_eq!(
                generation,
                metadata.generation,
                "{} operation generation changed",
                B::NAME
            );
        }
        if metadata.phase == Phase::Ready {
            metadata.phase = Phase::Consuming;
            Outcome::Ready {
                generation: metadata.generation,
                result: Error::from_code(metadata.result),
                rx: metadata.staging.rx,
                unused: waiter,
            }
        } else {
            let replaced = waiter.and_then(|waiter| metadata.waiter.replace(waiter));
            Outcome::Pending {
                generation: metadata.generation,
                replaced,
            }
        }
    }
}

pub(super) unsafe fn consumed<B: Bus>(header: NonNull<Header<B>>, generation: usize) {
    unsafe {
        let mut metadata = Locked::acquire(header);
        assert_eq!(
            generation,
            metadata.generation,
            "{} consumed another operation",
            B::NAME
        );
        metadata.phase = Phase::Idle;
    }
}
