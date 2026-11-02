// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

//! Owned asynchronous I2C with stable independent TX/RX staging.
//!
//! Ordinary Config and native-heap or claimed static constructors need no
//! application FFI, Rust allocator or manual pinning. Moving an owner leaves
//! its native controller, callback context and staging in place. Exclusive
//! operations borrow application slices only for live-future copy-in/copyback;
//! native code retains only internal memory. Drop or forget does not cancel I/O.
//!
//! Static capacities count bytes separately for TX and RX; exceeding either
//! returns NoSpace before work. Heap staging grows fallibly and retains each
//! phase's high-water capacity. Complete independent counts are preserved:
//! empty TX selects plain read, empty RX pure write, both empty InvalidParam.
//! No filler, chunking, timeout or automatic watchdog is introduced.
//!
//! A dropped/forgotten operation leaves at most one native operation to settle.
//! New work returns BusBusy while it is active. A new operation may replace an
//! already-Ready orphan; flush instead consumes its actual result without RX
//! copyback. Errors and released futures leave application RX unchanged. Owner
//! Drop defers deinit/reclamation until all callback/wake/submission tails retire.
//! Missing completion or a forgotten owner retains its bounded context/capacity
//! or static claim. Callbacks remain nonblocking task-context notifications.
//!
//! The embedded-hal-async seven-bit trait accepts an empty list as a no-op,
//! one nonempty write/read, or one nonempty write followed by one nonempty
//! read. Other complete lists return NotSupported before any transfer. An
//! empty single phase or a nonempty write with empty read returns InvalidParam; an empty
//! write prefix before a read returns NotSupported. Trait write_read uses
//! this same policy. Inherent write_read with empty TX remains a plain read.
//!
//! ```no_run
//! # async fn example() -> ove::Result<()> {
//! static BUS: ove::AsyncI2cStorage<3, 7> = ove::AsyncI2cStorage::new();
//! let mut bus = BUS.init(ove::i2c::Config::default())?;
//! let mut rx = [0; 7];
//! bus.write_read(0x42, &[1, 2, 3], &mut rx).await?;
//! bus.flush().await?;
//! # Ok(()) }
//! ```
//!
//! Physical-bus coordination with separate handles remains the caller's task.
//! Provider limits still apply (STM32 permits 65535 bytes per phase). NuttX debug
//! zero-heap preparation returns NotSupported; release zero-heap uses its
//! existing native worker reserve even with static Rust staging. Native worker
//! resource reclamation can outlast the final notification.

mod state;
mod transfer;

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::bindings;
use crate::error::{Error, Result};
use crate::i2c::Config;
use state::{Header, Staging};
pub use transfer::{Flush, Transfer};

/// Stable claimed storage with independent byte capacities per I2C phase.
/// RX defaults to TX for equal-sized staging. Either capacity may be zero.
/// Declare in a static; a live/forgotten owner or pending native tail retains
/// the claim. Initialization touches only the fixed header/native controller,
/// without constructing payload-sized runtime stack temporaries.
pub struct AsyncI2cStorage<const TX: usize, const RX: usize = TX> {
    header: UnsafeCell<MaybeUninit<Header>>,
    tx: UnsafeCell<[MaybeUninit<u8>; TX]>,
    rx: UnsafeCell<[MaybeUninit<u8>; RX]>,
    claimed: AtomicBool,
}

// SAFETY: the atomic claimant alone initializes backing. Internal native and
// callback lifetimes independently retain it; the final release completes
// native deinit and every backing access before reopening the static claim.
unsafe impl<const TX: usize, const RX: usize> Sync for AsyncI2cStorage<TX, RX> {}

impl<const TX: usize, const RX: usize> AsyncI2cStorage<TX, RX> {
    /// Reserve static backing without a runtime allocation.
    pub const fn new() -> Self {
        Self {
            header: UnsafeCell::new(MaybeUninit::uninit()),
            tx: UnsafeCell::new([MaybeUninit::uninit(); TX]),
            rx: UnsafeCell::new([MaybeUninit::uninit(); RX]),
            claimed: AtomicBool::new(false),
        }
    }

    /// Claim/configure one ordinary owner in either allocation mode.
    pub fn init(&'static self, config: Config) -> Result<AsyncI2c> {
        Error::from_code(unsafe { bindings::ove_i2c_async_prepare() })?;
        self.claimed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| Error::Busy)?;
        let header = unsafe { NonNull::new_unchecked(self.header.get().cast::<Header>()) };
        let staging = Staging {
            tx: self.tx.get().cast(),
            rx: self.rx.get().cast(),
            tx_capacity: TX,
            rx_capacity: RX,
            allocation: core::ptr::null_mut(),
        };
        // SAFETY: the successful static claim excludes all former owners and
        // tails. Native storage and the fixed header are initialized in place.
        unsafe { state::initialize(header, &self.claimed, staging, config) }?;
        Ok(AsyncI2c { header })
    }
}

impl<const TX: usize, const RX: usize> Default for AsyncI2cStorage<TX, RX> {
    fn default() -> Self {
        Self::new()
    }
}

/// Movable exclusive owner over stable internal backing; not Copy/Clone/Send/Sync.
/// Native descriptor portability across tasks/groups is not assumed.
pub struct AsyncI2c {
    header: NonNull<Header>,
}

impl core::fmt::Debug for AsyncI2c {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.debug_struct("AsyncI2c").finish_non_exhaustive()
    }
}

impl AsyncI2c {
    /// Allocate native backing without a Rust allocator.
    #[cfg(not(zero_heap))]
    pub fn new(config: Config) -> Result<Self> {
        Error::from_code(unsafe { bindings::ove_i2c_async_prepare() })?;
        let pointer =
            unsafe { bindings::ove_i2c_binding_alloc(size_of::<Header>(), align_of::<Header>()) };
        let header = NonNull::new(pointer.cast::<Header>()).ok_or(Error::NoMemory)?;
        // SAFETY: the checked native allocation stays stable through partial
        // init cleanup or final owner/callback/submission lifetime retirement.
        unsafe { state::initialize(header, core::ptr::null(), Staging::EMPTY, config) }?;
        Ok(Self { header })
    }

    /// One combined transaction with independent phase lengths and owned staging.
    /// Empty TX is plain read, empty RX pure write, both empty InvalidParam.
    pub fn write_read<'a>(
        &'a mut self,
        address: u16,
        tx: &'a [u8],
        rx: &'a mut [u8],
    ) -> Transfer<'a> {
        Transfer::duplex(self, address, tx, rx)
    }

    /// Send a nonempty byte slice in one native transaction.
    pub fn write<'a>(&'a mut self, address: u16, tx: &'a [u8]) -> Transfer<'a> {
        Transfer::write(self, address, tx)
    }

    /// Receive into a nonempty byte slice in one native transaction.
    pub fn read<'a>(&'a mut self, address: u16, rx: &'a mut [u8]) -> Transfer<'a> {
        Transfer::read(self, address, rx)
    }

    /// Await/consume an orphan's actual result, without application RX copyback.
    pub fn flush(&mut self) -> Flush<'_> {
        Flush::new(self)
    }
}

impl Drop for AsyncI2c {
    fn drop(&mut self) {
        // SAFETY: this is the sole public owner. Independent native/callback
        // and synchronous-call leases protect every remaining backing access.
        unsafe { state::close(self.header) };
    }
}

#[cfg(feature = "embedded-hal-async")]
mod hal_async_impl {
    use super::AsyncI2c;
    use crate::error::Error;
    use embedded_hal::i2c::{ErrorType, Operation, SevenBitAddress};
    use embedded_hal_async::i2c::I2c;

    impl ErrorType for AsyncI2c {
        type Error = Error;
    }
    impl I2c<SevenBitAddress> for AsyncI2c {
        async fn transaction(
            &mut self,
            address: u8,
            operations: &mut [Operation<'_>],
        ) -> Result<(), Error> {
            if address > 0x7f {
                return Err(Error::InvalidParam);
            }
            // Check the entire list before effects. Separate native calls
            // would introduce STOPs forbidden by the trait transaction contract.
            match operations {
                [] => Ok(()),
                [Operation::Write(write)] => {
                    if write.is_empty() {
                        return Err(Error::InvalidParam);
                    }
                    AsyncI2c::write(self, address.into(), write).await
                }
                [Operation::Read(read)] => {
                    if read.is_empty() {
                        return Err(Error::InvalidParam);
                    }
                    AsyncI2c::read(self, address.into(), read).await
                }
                [Operation::Write(write), Operation::Read(read)] => {
                    if write.is_empty() {
                        return Err(Error::NotSupported);
                    }
                    if read.is_empty() {
                        return Err(Error::InvalidParam);
                    }
                    AsyncI2c::write_read(self, address.into(), write, read).await
                }
                _ => Err(Error::NotSupported),
            }
        }
    }
}
