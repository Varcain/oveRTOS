// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Owned synchronous SPI controllers and borrowed transfer descriptions.
//!
//! [`Spi`] transfers `u8` words; [`Spi16`] transfers `u16` words. Safe
//! constructors configure the matching native word size. Heap construction
//! uses the native allocator; [`SpiStorage::init`] uses stable static backing
//! in either allocation mode. Neither requires a Rust allocator.
//!
//! ```no_run
//! use core::time::Duration;
//! use ove::{SpiStorage, spi::{Config, ChipSelect, Transfer}};
//! use ove::gpio::{GpioMode, OutputPin};
//!
//! static SPI: SpiStorage = SpiStorage::new();
//! # fn example() -> ove::Result<()> {
//! let mut select = OutputPin::new(0, 4, GpioMode::OutputPP)?;
//! select.set_high()?;
//! let cs = ChipSelect::new(0, 4, true);
//! let mut spi = SPI.init(Config::default())?;
//! let command = [0x9f];
//! let mut id = [0; 3];
//! spi.transfer_seq(Some(&cs), &mut [
//!     Transfer::write(&command), Transfer::read(&mut id),
//! ], Duration::from_millis(20))?;
//! # Ok(())
//! # }
//! ```
//!
//! Calls block in thread context. One sequence holds the supplied software
//! chip select and the mutex for its ove handle. Reserve/coordinate the
//! physical bus separately from other handles and native users, including
//! asynchronous operations. Native hardware selection/controller locks may
//! end between segments on NuttX/Zephyr. Unequal slices use up to two native
//! segments; each has its own timeout. Only STM32 FreeRTOS honors timeouts,
//! rounded down to milliseconds. Read filler is backend-defined and may
//! contain the initial RX contents. These APIs do not establish async
//! cancellation, retained-buffer ownership or callback retirement.

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::mem::{MaybeUninit, align_of, size_of, size_of_val};
use core::sync::atomic::{AtomicBool, Ordering};
use core::time::Duration;

use crate::bindings;
use crate::error::{Error, Result};

/// SPI clock mode (CPOL/CPHA).
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Mode {
    Mode0 = 0,
    Mode1 = 1,
    Mode2 = 2,
    Mode3 = 3,
}

/// SPI bit order.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum BitOrder {
    MsbFirst = 0,
    LsbFirst = 1,
}

/// Requested native controller settings. Word size comes from [`Spi`] or
/// [`Spi16`], rather than an independently mutable configuration field.
///
/// Backend capabilities still apply: STM32 currently uses a fixed prescaler
/// and NuttX does not apply the requested bit order. This config does not
/// reserve a physical peripheral against separately initialized handles.
#[derive(Debug, Copy, Clone)]
pub struct Config {
    pub instance: u32,
    pub clock_hz: u32,
    pub mode: Mode,
    pub bit_order: BitOrder,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            instance: 0,
            clock_hz: 1_000_000,
            mode: Mode::Mode0,
            bit_order: BitOrder::MsbFirst,
        }
    }
}

impl Config {
    fn native<W: Word>(self) -> bindings::ove_spi_cfg {
        bindings::ove_spi_cfg {
            instance: self.instance,
            clock_hz: self.clock_hz,
            mode: self.mode as bindings::ove_spi_mode_t,
            bit_order: self.bit_order as bindings::ove_spi_bit_order_t,
            word_size: W::BITS,
        }
    }
}

/// Software chip-select description. Configure the GPIO as an output before
/// use, for example with [`crate::gpio::OutputPin`]. This is not a pin owner.
#[repr(transparent)]
#[derive(Debug, Copy, Clone)]
pub struct ChipSelect {
    raw: bindings::ove_spi_cs,
}

impl ChipSelect {
    /// Describe a software-controlled GPIO chip select.
    pub const fn new(port: u32, pin: u32, active_low: bool) -> Self {
        Self {
            raw: bindings::ove_spi_cs {
                gpio_port: port,
                gpio_pin: pin,
                active_low: active_low as core::ffi::c_int,
            },
        }
    }
}

mod sealed {
    pub trait Sealed {}
    impl Sealed for u8 {}
    impl Sealed for u16 {}
}

/// Supported native SPI word types, sealed to `u8` and `u16`.
pub trait Word: sealed::Sealed + Copy + 'static {
    #[doc(hidden)]
    const BITS: u8;
}

impl Word for u8 {
    const BITS: u8 = 8;
}

impl Word for u16 {
    const BITS: u8 = 16;
}

/// Owned 8-bit synchronous SPI controller.
pub type Spi = Controller<u8>;
/// Owned 16-bit synchronous SPI controller.
pub type Spi16 = Controller<u16>;

/// Stable backing for a synchronous controller, normally declared in a static.
///
/// `init` claims this backing before native initialization. Drop deinitializes
/// before releasing that claim. Forgetting an owner retains the claim and its
/// stable native resource, so a later initialization returns [`Error::Busy`].
/// The native backing and claim cannot be accessed or moved through this API.
pub struct SpiStorage<W: Word = u8> {
    raw: UnsafeCell<MaybeUninit<bindings::ove_spi_storage_t>>,
    claimed: AtomicBool,
    word: PhantomData<W>,
}

// SAFETY: native backing is accessible only after the atomic claim succeeds.
// Its owner is not Send/Sync. Initialization failure releases the claim only
// after C cleanup returns; Drop releases it only after deinit returns. The
// static receiver keeps backing stable even when an owner is forgotten.
unsafe impl<W: Word> Sync for SpiStorage<W> {}

impl<W: Word> SpiStorage<W> {
    /// Reserve uninitialized controller backing without a runtime allocation.
    pub const fn new() -> Self {
        Self {
            raw: UnsafeCell::new(MaybeUninit::zeroed()),
            claimed: AtomicBool::new(false),
            word: PhantomData,
        }
    }

    /// Initialize one owner in stable static backing. Available in heap and
    /// zero-heap builds; applications need no raw pointer or pinning operation.
    pub fn init(&'static self, config: Config) -> Result<Controller<W>> {
        self.claimed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| Error::Busy)?;
        let mut handle = core::ptr::null_mut();
        let native = config.native::<W>();
        // SAFETY: the claim gives exclusive access to correctly sized/aligned
        // static backing. C initializes it without requiring a Rust value.
        let rc = unsafe { bindings::ove_spi_init(&mut handle, self.raw.get().cast(), &native) };
        if let Err(error) = Error::from_code(rc) {
            // C may publish an out pointer before failing; it has already
            // cleaned its partial initialization. No owner exists on error.
            self.claimed.store(false, Ordering::Release);
            return Err(error);
        }
        Ok(Controller {
            handle,
            ownership: Ownership::Static(self),
            word: PhantomData,
        })
    }
}

impl<W: Word> Default for SpiStorage<W> {
    fn default() -> Self {
        Self::new()
    }
}

enum Ownership<W: Word> {
    #[cfg(not(zero_heap))]
    Heap,
    Static(&'static SpiStorage<W>),
    Borrowed,
}

/// Synchronous controller for a sealed native word type. Prefer [`Spi`] and
/// [`Spi16`] for ordinary construction. It owns its native resource, except
/// when explicitly created with unsafe nonowning [`Self::from_handle`].
///
/// Moving an owner does not move native backing. Copy/Clone and Send/Sync are
/// deliberately absent; native descriptor/task-group portability is not assumed.
pub struct Controller<W: Word> {
    handle: bindings::ove_spi_t,
    ownership: Ownership<W>,
    word: PhantomData<W>,
}

impl<W: Word> core::fmt::Debug for Controller<W> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Spi")
            .field("handle", &self.handle)
            .field("word_size", &W::BITS)
            .finish_non_exhaustive()
    }
}

impl<W: Word> Controller<W> {
    /// Allocate and configure a native controller without requiring Rust alloc.
    ///
    /// ```no_run
    /// # #[cfg(not(zero_heap))]
    /// # fn example() -> ove::Result<()> {
    /// use core::time::Duration;
    /// use ove::{Spi16, spi::Config};
    /// let mut spi = Spi16::new(Config::default())?;
    /// spi.transfer_in_place(None, &mut [0x1234u16; 2], Duration::from_millis(20))?;
    /// # Ok(())
    /// # }
    /// ```
    #[cfg(not(zero_heap))]
    pub fn new(config: Config) -> Result<Self> {
        let mut handle = core::ptr::null_mut();
        let native = config.native::<W>();
        // SAFETY: C copies the numeric configuration and owns allocation and
        // failed-create cleanup. Only a successful result creates this owner.
        Error::from_code(unsafe { bindings::ove_spi_create(&mut handle, &native) })?;
        Ok(Self {
            handle,
            ownership: Ownership::Heap,
            word: PhantomData,
        })
    }

    /// Wrap an externally owned handle. Drop does not deinitialize or free it.
    ///
    /// # Safety
    /// The handle must be live, non-NULL and configured for `W::BITS`. Its
    /// backing/configuration must remain stable for the wrapper's lifetime.
    /// Coordinate all external access, reconfiguration and destruction, and
    /// any native asynchronous operation retaining this handle or its buffers.
    /// This constructor does not transfer ownership or establish async safety.
    pub const unsafe fn from_handle(handle: bindings::ove_spi_t) -> Self {
        Self {
            handle,
            ownership: Ownership::Borrowed,
            word: PhantomData,
        }
    }

    /// Obtain a nonowning raw handle for explicit unsafe native integration.
    /// It grants no lifetime extension, ownership transfer or async guarantee.
    pub fn raw(&self) -> bindings::ove_spi_t {
        self.handle
    }

    /// Clock the larger word count. Transfer the common prefix full-duplex,
    /// then discard excess RX or use backend-defined TX filler for the tail.
    pub fn transfer(
        &mut self,
        cs: Option<&ChipSelect>,
        tx: &[W],
        rx: &mut [W],
        timeout: Duration,
    ) -> Result<()> {
        self.transfer_seq(cs, &mut [Transfer::duplex(tx, rx)], timeout)
    }

    /// Write words; empty slices succeed without native or CS activity.
    pub fn write(&mut self, cs: Option<&ChipSelect>, tx: &[W], timeout: Duration) -> Result<()> {
        self.transfer_seq(cs, &mut [Transfer::write(tx)], timeout)
    }

    /// Read words with backend-defined transmit filler. Empty reads are no-ops.
    pub fn read(&mut self, cs: Option<&ChipSelect>, rx: &mut [W], timeout: Duration) -> Result<()> {
        self.transfer_seq(cs, &mut [Transfer::read(rx)], timeout)
    }

    /// Replace each word with its received value using exact native in-place I/O.
    pub fn transfer_in_place(
        &mut self,
        cs: Option<&ChipSelect>,
        words: &mut [W],
        timeout: Duration,
    ) -> Result<()> {
        self.transfer_seq(cs, &mut [Transfer::in_place(words)], timeout)
    }

    /// Submit borrowed descriptions in one native sequence. The supplied
    /// software CS and same-handle mutex span it; native hardware ownership
    /// may end between segments. Every native segment has its own timeout.
    /// Empty arrays and arrays containing only empty entries are no-ops.
    pub fn transfer_seq(
        &mut self,
        cs: Option<&ChipSelect>,
        transfers: &mut [Transfer<'_, W>],
        timeout: Duration,
    ) -> Result<()> {
        const {
            assert!(size_of::<Transfer<'static, W>>() == size_of::<[bindings::ove_spi_xfer; 2]>());
            assert!(
                align_of::<Transfer<'static, W>>() == align_of::<[bindings::ove_spi_xfer; 2]>()
            );
        }
        if transfers.iter().all(Transfer::is_empty) {
            return Ok(());
        }
        let count = native_record_count(transfers.len())?;
        let cs = cs.map_or(core::ptr::null(), |value| &raw const value.raw);
        // SAFETY: transparent descriptors have the exact array stride/alignment
        // above. Checked count covers both records per entry. Their exclusive
        // borrow retains every original data borrow through this synchronous
        // call; C reads records and accesses only their bounded data spans.
        let rc = unsafe {
            bindings::ove_spi_transfer_seq(
                self.handle,
                cs,
                transfers.as_ptr().cast(),
                count,
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }
}

impl<W: Word> Drop for Controller<W> {
    fn drop(&mut self) {
        match &self.ownership {
            #[cfg(not(zero_heap))]
            Ownership::Heap => {
                // SAFETY: this variant exists only after successful C create;
                // safe synchronous operations retain no backing after return.
                unsafe { bindings::ove_spi_destroy(self.handle) };
            }
            Ownership::Static(storage) => {
                // SAFETY: this owner holds the static claim and native init
                // succeeded. Complete native cleanup before permitting reuse.
                unsafe { bindings::ove_spi_deinit(self.handle) };
                storage.claimed.store(false, Ordering::Release);
            }
            Ownership::Borrowed => {}
        }
    }
}

/// Borrowed SPI operation with private native records. It retains shared TX
/// and exclusive RX borrows through its last use and cannot be copied/cloned.
/// `u16` descriptions belong only to a [`Spi16`] controller.
#[repr(transparent)]
pub struct Transfer<'a, W: Word = u8> {
    raw: [bindings::ove_spi_xfer; 2],
    borrow: PhantomData<&'a mut [W]>,
}

fn empty_record() -> bindings::ove_spi_xfer {
    bindings::ove_spi_xfer {
        tx: core::ptr::null(),
        rx: core::ptr::null_mut(),
        len: 0,
    }
}

impl<'a, W: Word> Transfer<'a, W> {
    /// Describe a write without copying the data.
    pub fn write(tx: &'a [W]) -> Self {
        let mut raw = [empty_record(), empty_record()];
        if !tx.is_empty() {
            raw[0].tx = tx.as_ptr().cast();
            raw[0].len = size_of_val(tx);
        }
        Self {
            raw,
            borrow: PhantomData,
        }
    }

    /// Describe a read, exclusively borrowing its backing.
    pub fn read(rx: &'a mut [W]) -> Self {
        let mut raw = [empty_record(), empty_record()];
        if !rx.is_empty() {
            raw[0].rx = rx.as_mut_ptr().cast();
            raw[0].len = size_of_val(rx);
        }
        Self {
            raw,
            borrow: PhantomData,
        }
    }

    /// Describe unequal or equal full-duplex spans. At most two bounded native
    /// records represent the common prefix and the half-duplex remainder.
    pub fn duplex(tx: &'a [W], rx: &'a mut [W]) -> Self {
        let common = tx.len().min(rx.len());
        let (prefix_tx, tail_tx) = tx.split_at(common);
        let (prefix_rx, tail_rx) = rx.split_at_mut(common);
        let mut raw = [empty_record(), empty_record()];
        if common != 0 {
            raw[0].tx = prefix_tx.as_ptr().cast();
            raw[0].rx = prefix_rx.as_mut_ptr().cast();
            raw[0].len = size_of_val(prefix_tx);
        }
        if !tail_tx.is_empty() {
            raw[1].tx = tail_tx.as_ptr().cast();
            raw[1].len = size_of_val(tail_tx);
        } else if !tail_rx.is_empty() {
            raw[1].rx = tail_rx.as_mut_ptr().cast();
            raw[1].len = size_of_val(tail_rx);
        }
        Self {
            raw,
            borrow: PhantomData,
        }
    }

    /// Describe exact in-place I/O from a single exclusive borrow.
    pub fn in_place(words: &'a mut [W]) -> Self {
        let mut raw = [empty_record(), empty_record()];
        if !words.is_empty() {
            let pointer = words.as_mut_ptr();
            raw[0].tx = pointer.cast();
            raw[0].rx = pointer.cast();
            raw[0].len = size_of_val(words);
        }
        Self {
            raw,
            borrow: PhantomData,
        }
    }

    fn is_empty(&self) -> bool {
        self.raw.iter().all(|record| record.len == 0)
    }
}

// Valid u8/u16 slices already guarantee a representable allocation byte size;
// size_of_val derives counts without unchecked word-to-byte arithmetic. Only
// the independently supplied number of descriptors needs checked expansion.
fn native_record_count(logical: usize) -> Result<core::ffi::c_uint> {
    let expanded = logical.checked_mul(2).ok_or(Error::InvalidParam)?;
    expanded.try_into().map_err(|_| Error::InvalidParam)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_record_count_checks_expansion_and_abi_limit() {
        let maximum = core::ffi::c_uint::MAX as usize;
        assert_eq!(native_record_count(0), Ok(0));
        assert_eq!(
            native_record_count(maximum / 2),
            Ok(core::ffi::c_uint::MAX - 1)
        );
        assert_eq!(
            native_record_count(maximum / 2 + 1),
            Err(Error::InvalidParam)
        );
        assert_eq!(
            native_record_count(usize::MAX / 2 + 1),
            Err(Error::InvalidParam)
        );
        assert_eq!(native_record_count(usize::MAX), Err(Error::InvalidParam));
    }
}
