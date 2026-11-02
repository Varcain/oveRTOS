// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::mem::MaybeUninit;
use core::ops::{Deref, DerefMut};
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering, fence};
use core::task::Waker;

use crate::bindings;
use crate::error::{Error, Result};
use crate::spi::{ChipSelect, Config, Word};

#[derive(Copy, Clone)]
pub(super) struct Staging {
    pub tx: *mut u8,
    pub rx: *mut u8,
    pub capacity: usize,
    pub allocation: *mut c_void,
}

impl Staging {
    #[cfg(not(zero_heap))]
    pub const EMPTY: Self = Self {
        tx: ptr::null_mut(),
        rx: ptr::null_mut(),
        capacity: 0,
        allocation: ptr::null_mut(),
    };
}

#[derive(Copy, Clone, Eq, PartialEq)]
enum Phase {
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

pub(super) struct Header {
    refs: AtomicUsize,
    locked: AtomicBool,
    native: UnsafeCell<MaybeUninit<bindings::ove_spi_storage_t>>,
    cs: UnsafeCell<MaybeUninit<bindings::ove_spi_cs>>,
    handle: bindings::ove_spi_t,
    claim: *const AtomicBool,
    metadata: UnsafeCell<Metadata>,
}

/// A synchronous call or native/callback lifetime, never stored in a Future.
pub(super) struct Lease {
    header: NonNull<Header>,
}

impl Lease {
    pub unsafe fn acquire(header: NonNull<Header>) -> Self {
        // SAFETY: the caller already owns a live reference. A checked CAS
        // excludes overflow rather than incrementing and checking afterward.
        unsafe {
            (*header.as_ptr())
                .refs
                .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
                    n.checked_add(1)
                        .filter(|next| n != 0 && *next <= isize::MAX as usize)
                })
                .expect("SPI lifetime count overflow");
        }
        Self { header }
    }

    unsafe fn adopt(header: NonNull<Header>) -> Self {
        Self { header }
    }
}

impl Drop for Lease {
    fn drop(&mut self) {
        // SAFETY: this local owns exactly one count and never accesses the
        // header after releasing it. Final cleanup may reclaim its allocation.
        unsafe { release(self.header) };
    }
}

struct Locked {
    header: NonNull<Header>,
    irq: bindings::ove_irq_key_t,
}

impl Locked {
    unsafe fn acquire(header: NonNull<Header>) -> Self {
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

impl Deref for Locked {
    type Target = Metadata;
    fn deref(&self) -> &Metadata {
        // SAFETY: the lock serializes this interior-mutable metadata only.
        unsafe { &*(*self.header.as_ptr()).metadata.get() }
    }
}
impl DerefMut for Locked {
    fn deref_mut(&mut self) -> &mut Metadata {
        // SAFETY: this guard has exclusive metadata access until it unlocks.
        unsafe { &mut *(*self.header.as_ptr()).metadata.get() }
    }
}
impl Drop for Locked {
    fn drop(&mut self) {
        unsafe {
            (*self.header.as_ptr())
                .locked
                .store(false, Ordering::Release);
            bindings::ove_irq_unlock(self.irq);
        }
    }
}

pub(super) unsafe fn initialize<W: Word>(
    header: NonNull<Header>,
    claim: *const AtomicBool,
    staging: Staging,
    config: Config,
) -> Result<()> {
    // SAFETY: allocation/claim gives exclusive uninitialized backing. Write
    // fixed metadata in place; never create a payload-sized stack temporary.
    unsafe {
        let raw = header.as_ptr();
        ptr::addr_of_mut!((*raw).refs).write(AtomicUsize::new(1));
        ptr::addr_of_mut!((*raw).locked).write(AtomicBool::new(false));
        ptr::addr_of_mut!((*raw).native).write(UnsafeCell::new(MaybeUninit::uninit()));
        ptr::addr_of_mut!((*raw).cs).write(UnsafeCell::new(MaybeUninit::uninit()));
        ptr::addr_of_mut!((*raw).handle).write(ptr::null_mut());
        ptr::addr_of_mut!((*raw).claim).write(claim);
        ptr::addr_of_mut!((*raw).metadata).write(UnsafeCell::new(Metadata {
            phase: Phase::Idle,
            generation: 0,
            result: 0,
            closing: false,
            waiter: None,
            staging,
        }));
        let native = config.native::<W>();
        let mut handle = ptr::null_mut();
        let result = bindings::ove_spi_init(&mut handle, (*raw).native.get().cast(), &native);
        if let Err(error) = Error::from_code(result) {
            // C already cleaned partial initialization; handle remains NULL
            // here, so final cleanup must not deinitialize the failed out pointer.
            release(header);
            return Err(error);
        }
        (*raw).handle = handle;
    }
    Ok(())
}

unsafe fn release(header: NonNull<Header>) {
    unsafe {
        let raw = header.as_ptr();
        if (*raw).refs.fetch_sub(1, Ordering::Release) != 1 {
            return;
        }
        fence(Ordering::Acquire);
        // The unique last-reference winner owns cleanup through native close
        // returning. No callback, poll or submit call can still use the header.
        let handle = (*raw).handle;
        let claim = (*raw).claim;
        let metadata = &mut *(*raw).metadata.get();
        let waiter = metadata.waiter.take();
        let allocation = metadata.staging.allocation;
        drop(waiter);
        if !handle.is_null() {
            bindings::ove_spi_deinit(handle);
        }
        #[cfg(not(zero_heap))]
        if !allocation.is_null() {
            bindings::ove_spi_binding_free(allocation);
        }
        #[cfg(zero_heap)]
        let _ = allocation;
        ptr::drop_in_place(raw);
        if claim.is_null() {
            #[cfg(not(zero_heap))]
            bindings::ove_spi_binding_free(raw.cast());
        } else {
            // This is the final storage access; another initializer may now
            // overwrite the header. No destructor/lock guard survives it.
            (*claim).store(false, Ordering::Release);
        }
    }
}

pub(super) unsafe fn close(header: NonNull<Header>) {
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

pub(super) unsafe fn detach(header: NonNull<Header>, generation: usize) {
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

unsafe extern "C" fn completed(result: core::ffi::c_int, user: *mut c_void) {
    // SAFETY: accepted submission transfers one context count to this callback.
    // It becomes a wake-tail count; no Future Drop is needed to release it.
    let header = unsafe { NonNull::new_unchecked(user.cast::<Header>()) };
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

pub(super) unsafe fn begin<W: Word>(
    header: NonNull<Header>,
    tx: *const W,
    tx_words: usize,
    words: usize,
    cs: Option<ChipSelect>,
    waiter: Waker,
) -> Result<usize> {
    unsafe {
        let bytes = words
            .checked_mul(size_of::<W>())
            .filter(|n| *n <= isize::MAX as usize)
            .ok_or(Error::InvalidParam)?;
        let (previous, old_stage) = {
            let metadata = Locked::acquire(header);
            if metadata.closing
                || matches!(
                    metadata.phase,
                    Phase::Active | Phase::Preparing | Phase::Consuming
                )
            {
                return Err(Error::BusBusy);
            }
            if !(*header.as_ptr()).claim.is_null() && words > metadata.staging.capacity {
                return Err(Error::NoSpace);
            }
            (metadata.phase, metadata.staging)
        };
        Error::from_code(bindings::ove_spi_binding_check_length(
            (*header.as_ptr()).handle,
            bytes,
        ))?;
        {
            let mut metadata = Locked::acquire(header);
            metadata.phase = Phase::Preparing;
        }
        let stage_result = grow::<W>(header, old_stage, words);
        let stage = match stage_result {
            Ok(stage) => stage,
            Err(error) => {
                Locked::acquire(header).phase = previous;
                return Err(error);
            }
        };
        ptr::write_bytes(stage.tx, 0, bytes);
        ptr::write_bytes(stage.rx, 0, bytes);
        if tx_words != 0 {
            ptr::copy_nonoverlapping(tx, stage.tx.cast::<W>(), tx_words);
        }
        // CS has its own interior-mutable region. Metadata's short mutable
        // borrows must not alias a descriptor being read by native completion.
        let chip_select = match cs {
            Some(value) => {
                let target = (*header.as_ptr()).cs.get().cast::<bindings::ove_spi_cs>();
                target.write(value.native());
                target as *const _
            }
            None => ptr::null(),
        };
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
        let rc = bindings::ove_spi_transfer_async(
            (*header.as_ptr()).handle,
            chip_select,
            stage.tx.cast(),
            stage.rx.cast(),
            bytes,
            Some(completed),
            header.as_ptr().cast(),
        );
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

unsafe fn grow<W: Word>(header: NonNull<Header>, old: Staging, words: usize) -> Result<Staging> {
    if words <= old.capacity {
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
        let count = words.checked_mul(2).ok_or(Error::InvalidParam)?;
        let layout = core::alloc::Layout::array::<W>(count).map_err(|_| Error::InvalidParam)?;
        let allocation = bindings::ove_spi_binding_alloc(layout.size(), layout.align());
        if allocation.is_null() {
            return Err(Error::NoMemory);
        }
        let stage = Staging {
            tx: allocation.cast(),
            rx: allocation.cast::<W>().add(words).cast(),
            capacity: words,
            allocation,
        };
        // No native operation retains old staging: Ready precedes this path,
        // and every old callback uses only its context lease after publication.
        if !old.allocation.is_null() {
            bindings::ove_spi_binding_free(old.allocation);
        }
        Ok(stage)
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

pub(super) unsafe fn observe(
    header: NonNull<Header>,
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
                generation, metadata.generation,
                "SPI operation generation changed"
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

pub(super) unsafe fn consumed(header: NonNull<Header>, generation: usize) {
    unsafe {
        let mut metadata = Locked::acquire(header);
        assert_eq!(
            generation, metadata.generation,
            "SPI consumed another operation"
        );
        metadata.phase = Phase::Idle;
    }
}
