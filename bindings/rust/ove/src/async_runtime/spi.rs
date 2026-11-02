// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

//! Owned asynchronous SPI controllers with stable operation memory.
//!
//! [`AsyncSpi`] clocks bytes; [`AsyncSpi16`] clocks `u16` words. Native I/O
//! retains only internal TX/RX staging and a copied chip-select description.
//! Dropping or forgetting an operation does not cancel I/O, but never leaves
//! native code holding application buffers or the future's address.
//!
//! Static capacity counts words and applies to the complete operation. A
//! request exceeding it returns [`Error::NoSpace`]. Heap owners grow staging
//! fallibly through the native allocator, without a Rust allocator. Transfers
//! clock the larger slice length, pad missing TX words with zero, and discard
//! excess RX. Successful live futures copy only their requested RX words back;
//! errors leave application RX unchanged. In-place transfers use the same
//! full-operation staging, without automatic chunking or a special length cap.
//!
//! A forgotten operation leaves one native operation to settle. A new nonempty
//! operation returns [`Error::BusBusy`] while it remains active; [`Controller::flush`]
//! awaits it and propagates its completion error. Empty operations have no
//! native effects and do not flush it. Owner Drop defers native teardown until
//! all callback and submission tails retire. A missing completion or forgotten
//! owner intentionally retains its bounded context/capacity or static claim.
//!
//! Native frame limits and physical-bus coordination still apply. STM32 accepts
//! at most 65535 frames per submission. NuttX workers allocate native TCB/stack
//! memory even with static staging; debug zero-heap construction returns
//! [`Error::NotSupported`]. Release zero-heap uses its existing bounded reserve.

mod state;
mod transfer;

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::mem::MaybeUninit;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::bindings;
use crate::error::{Error, Result};
use crate::spi::{ChipSelect, Config, Word};
use state::{Header, Staging};
pub use transfer::{Flush, Transfer};

/// Owned asynchronous byte controller.
pub type AsyncSpi = Controller<u8>;
/// Owned asynchronous 16-bit-word controller.
pub type AsyncSpi16 = Controller<u16>;

/// Stable static backing with capacity for `N` complete transfer words.
///
/// Available in both allocation modes. Declare it in a static; initialization
/// atomically claims it. Neither moving an owner nor forgetting a future moves
/// its backing. Reinitialization remains Busy until native deinit and every
/// old callback/wake/submission tail finish. Runtime initialization writes only
/// the small header and native controller, leaving large staging arrays in place.
pub struct AsyncSpiStorage<const N: usize, W: Word = u8> {
    header: UnsafeCell<MaybeUninit<Header>>,
    tx: UnsafeCell<[MaybeUninit<W>; N]>,
    rx: UnsafeCell<[MaybeUninit<W>; N]>,
    claimed: AtomicBool,
}

// SAFETY: only the successful atomic claimant initializes/accesses backing.
// Native and callback access is internal and independently leased; metadata
// uses a cross-core lock. The final lease releases the claim only after deinit
// and all backing accesses, so another claimant cannot overwrite live state.
unsafe impl<const N: usize, W: Word> Sync for AsyncSpiStorage<N, W> {}

impl<const N: usize, W: Word> AsyncSpiStorage<N, W> {
    /// Reserve static backing without a runtime allocator.
    pub const fn new() -> Self {
        Self {
            header: UnsafeCell::new(MaybeUninit::uninit()),
            tx: UnsafeCell::new([MaybeUninit::uninit(); N]),
            rx: UnsafeCell::new([MaybeUninit::uninit(); N]),
            claimed: AtomicBool::new(false),
        }
    }

    /// Configure one typed owner; no FFI, unsafe constructor or manual pinning.
    pub fn init(&'static self, config: Config) -> Result<Controller<W>> {
        Error::from_code(unsafe { bindings::ove_spi_async_prepare() })?;
        self.claimed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| Error::Busy)?;
        let header = unsafe { NonNull::new_unchecked(self.header.get().cast::<Header>()) };
        let staging = Staging {
            tx: self.tx.get().cast(),
            rx: self.rx.get().cast(),
            capacity: N,
            allocation: core::ptr::null_mut(),
        };
        // SAFETY: the static claim excludes every previous owner/tail. Native
        // initialization and the small header are written in place.
        unsafe { state::initialize::<W>(header, &self.claimed, staging, config) }?;
        Ok(Controller {
            header,
            word: PhantomData,
        })
    }
}

impl<const N: usize, W: Word> Default for AsyncSpiStorage<N, W> {
    fn default() -> Self {
        Self::new()
    }
}

/// Movable owner over stable native/context backing. It is neither Copy/Clone
/// nor Send/Sync; native descriptors are not assumed portable between tasks.
pub struct Controller<W: Word> {
    header: NonNull<Header>,
    word: PhantomData<W>,
}

impl<W: Word> core::fmt::Debug for Controller<W> {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter
            .debug_struct("AsyncSpi")
            .field("word_bits", &W::BITS)
            .finish_non_exhaustive()
    }
}

impl<W: Word> Controller<W> {
    /// Allocate stable backing through the configured native allocator.
    #[cfg(not(zero_heap))]
    pub fn new(config: Config) -> Result<Self> {
        Error::from_code(unsafe { bindings::ove_spi_async_prepare() })?;
        let pointer =
            unsafe { bindings::ove_spi_binding_alloc(size_of::<Header>(), align_of::<Header>()) };
        let header = NonNull::new(pointer.cast::<Header>()).ok_or(Error::NoMemory)?;
        // SAFETY: allocation has the checked header size/alignment, remains
        // stable and is reclaimed on failed init or final lease retirement.
        unsafe { state::initialize::<W>(header, core::ptr::null(), Staging::EMPTY, config) }?;
        Ok(Self {
            header,
            word: PhantomData,
        })
    }

    /// Clock max(tx.len(), rx.len()) words using owned staging and one CS interval.
    pub fn transfer<'a>(
        &'a mut self,
        cs: Option<&ChipSelect>,
        tx: &'a [W],
        rx: &'a mut [W],
    ) -> Transfer<'a, W> {
        Transfer::duplex(self, cs.copied(), tx, rx)
    }

    /// Send words; received words are discarded.
    pub fn write<'a>(&'a mut self, cs: Option<&ChipSelect>, tx: &'a [W]) -> Transfer<'a, W> {
        Transfer::write(self, cs.copied(), tx)
    }

    /// Receive words while transmitting zero filler.
    pub fn read<'a>(&'a mut self, cs: Option<&ChipSelect>, rx: &'a mut [W]) -> Transfer<'a, W> {
        Transfer::read(self, cs.copied(), rx)
    }

    /// Stage and replace every word in one native operation, at any admitted length.
    pub fn transfer_in_place<'a>(
        &'a mut self,
        cs: Option<&ChipSelect>,
        words: &'a mut [W],
    ) -> Transfer<'a, W> {
        Transfer::in_place(self, cs.copied(), words)
    }

    /// Await an outstanding orphan, consume its result and propagate its error.
    pub fn flush(&mut self) -> Flush<'_, W> {
        Flush::new(self)
    }
}

impl<W: Word> Drop for Controller<W> {
    fn drop(&mut self) {
        // SAFETY: this is the sole public owner reference. Native operation,
        // callback and synchronous-call leases independently preserve backing.
        unsafe { state::close(self.header) };
    }
}

#[cfg(feature = "embedded-hal-async")]
mod hal_async_impl {
    use super::Controller;
    use crate::error::Error;
    use crate::spi::Word;
    use embedded_hal::spi::ErrorType;
    use embedded_hal_async::spi::SpiBus;

    impl<W: Word> ErrorType for Controller<W> {
        type Error = Error;
    }
    impl<W: Word> SpiBus<W> for Controller<W> {
        async fn read(&mut self, words: &mut [W]) -> Result<(), Error> {
            Controller::read(self, None, words).await
        }
        async fn write(&mut self, words: &[W]) -> Result<(), Error> {
            Controller::write(self, None, words).await
        }
        async fn transfer(&mut self, read: &mut [W], write: &[W]) -> Result<(), Error> {
            Controller::transfer(self, None, write, read).await
        }
        async fn transfer_in_place(&mut self, words: &mut [W]) -> Result<(), Error> {
            Controller::transfer_in_place(self, None, words).await
        }
        async fn flush(&mut self) -> Result<(), Error> {
            Controller::flush(self).await
        }
    }
}
