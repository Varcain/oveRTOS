// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! I2C bus master driver.
//!
//! Safe constructors own a native controller: `I2c::new` uses the native
//! allocator and [`I2cStorage::init`] claims stable static backing in either
//! allocation mode. No Rust allocator, application FFI or pinning is required.
//! Operations and destruction run in thread context, with exclusive bus access
//! through each owner. Reserve/coordinate the physical bus separately from
//! independently configured handles and native users.
//!
//! With `embedded-hal`, transactions support an empty operation list, one
//! write (including zero bytes), one nonempty read, or a nonempty write followed
//! by a nonempty read. Other shapes fail before bus effects. An empty write
//! prefix followed by read is unsupported because it needs two address phases;
//! inherent [`I2c::write_read`] retains its empty-prefix plain-read convenience.
//!
//! Timeouts are honoured only by the FreeRTOS STM32 driver, in whole
//! milliseconds rounded down; Zephyr and NuttX ignore them. After validation,
//! synchronous calls wait without a timeout for other transfers through the same handle.
//!
//! Seven-bit addresses (0..127) and independent TX/RX byte counts are validated
//! before bus effects. Trait transactions validate the address even for an
//! empty list, without a native call. STM32 permits 65535 bytes per phase, Zephyr a `uint32_t`
//! count, and NuttX the smaller of `ssize_t` and `INT_MAX`. Requests are not split
//! to bypass these limits. Register writes include the prefix byte; zero-byte
//! writes/probes preserve their native address/ACK path.

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::bindings;
use crate::error::{Error, Result};

// Safe constructors establish owned, stable native backing. Synchronous calls
// borrow complete Rust spans through return; unsafe nonowning integration must
// uphold the validity, exclusivity and quiescence contract below.

/// I2C bus speed grade.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Speed {
    /// Standard mode — 100 kHz.
    Standard = 0,
    /// Fast mode — 400 kHz.
    Fast = 1,
    /// Fast-mode Plus — 1 MHz.
    FastPlus = 2,
}

/// Native controller configuration. This does not reserve a physical bus
/// against separately initialized handles or other native users.
#[derive(Debug, Copy, Clone)]
pub struct Config {
    pub instance: u32,
    pub speed: Speed,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            instance: 0,
            speed: Speed::Standard,
        }
    }
}

impl Config {
    pub(crate) fn native(self) -> bindings::ove_i2c_cfg {
        bindings::ove_i2c_cfg {
            instance: self.instance,
            speed: self.speed as bindings::ove_i2c_speed_t,
        }
    }
}

/// Stable claimed backing for one synchronous owner, normally a static.
/// Drop completes native deinit before releasing the claim. Forgetting an
/// owner retains the claim and its resource; initialization then returns Busy.
pub struct I2cStorage {
    raw: UnsafeCell<MaybeUninit<bindings::ove_i2c_storage_t>>,
    claimed: AtomicBool,
}

// SAFETY: the atomic claim gives one owner access to stable native backing.
// Owners are not Send/Sync; failed initialization and Drop release the claim
// only after native cleanup returns. Forgotten owners retain the claim.
unsafe impl Sync for I2cStorage {}

impl I2cStorage {
    /// Reserve native backing without a runtime allocation.
    pub const fn new() -> Self {
        Self {
            raw: UnsafeCell::new(MaybeUninit::zeroed()),
            claimed: AtomicBool::new(false),
        }
    }

    /// Initialize in stable static backing, in heap or zero-heap builds.
    ///
    /// ```no_run
    /// use core::time::Duration;
    /// use ove::{I2cStorage, i2c::Config};
    /// static BUS: I2cStorage = I2cStorage::new();
    /// # fn example() -> ove::Result<()> {
    /// let mut bus = BUS.init(Config::default())?;
    /// bus.reg_read(0x42, 0x10, &mut [0; 3], Duration::from_millis(20))?;
    /// # Ok(())
    /// # }
    /// ```
    pub fn init(&'static self, config: Config) -> Result<I2c> {
        self.claimed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| Error::Busy)?;
        let mut handle = core::ptr::null_mut();
        let native = config.native();
        // SAFETY: the claim provides exclusive correctly sized/aligned static
        // backing. No initialized Rust native record is required by C.
        let rc = unsafe { bindings::ove_i2c_init(&mut handle, self.raw.get().cast(), &native) };
        if let Err(error) = Error::from_code(rc) {
            // A non-NULL error out pointer is not an owner. C has completed
            // partial cleanup before returning, so the claim can now reopen.
            self.claimed.store(false, Ordering::Release);
            return Err(error);
        }
        Ok(I2c {
            handle,
            ownership: Ownership::Static(self),
        })
    }
}

impl Default for I2cStorage {
    fn default() -> Self {
        Self::new()
    }
}

enum Ownership {
    #[cfg(not(zero_heap))]
    Heap,
    Static(&'static I2cStorage),
    Borrowed,
}

/// Synchronous I2C owner. Moving it leaves native backing in place.
/// Copy/Clone and Send/Sync are deliberately absent; native task-group and
/// descriptor portability are not assumed. Only unsafe `from_handle` is nonowning.
pub struct I2c {
    handle: bindings::ove_i2c_t,
    ownership: Ownership,
}

impl core::fmt::Debug for I2c {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("I2c")
            .field("handle", &self.handle)
            .finish_non_exhaustive()
    }
}

impl I2c {
    /// Allocate/configure through the native allocator, without Rust alloc.
    ///
    /// ```no_run
    /// # #[cfg(not(zero_heap))]
    /// # fn example() -> ove::Result<()> {
    /// use core::time::Duration;
    /// use ove::{I2c, i2c::Config};
    /// let mut bus=I2c::new(Config::default())?;
    /// bus.write(0x42,&[1,2],Duration::from_millis(20))?;
    /// # Ok(())
    /// # }
    /// ```
    #[cfg(not(zero_heap))]
    pub fn new(config: Config) -> Result<Self> {
        let mut handle = core::ptr::null_mut();
        let native = config.native();
        // SAFETY: C copies this numeric config and owns failed-create cleanup.
        // Only success creates an owner, even if failure publishes an out pointer.
        Error::from_code(unsafe { bindings::ove_i2c_create(&mut handle, &native) })?;
        Ok(Self {
            handle,
            ownership: Ownership::Heap,
        })
    }

    /// Wrap an external native owner without taking lifecycle ownership.
    ///
    /// # Safety
    /// The non-NULL handle/backing must stay configured and valid through every
    /// use. Coordinate all aliases, reconfiguration and destruction; no other
    /// wrapper may provide conflicting access. Complete external native async
    /// work and all API entries before releasing/reusing its backing. This
    /// wrapper's Drop does not deinitialize or destroy the external resource.
    #[inline]
    pub const unsafe fn from_handle(handle: bindings::ove_i2c_t) -> Self {
        Self {
            handle,
            ownership: Ownership::Borrowed,
        }
    }

    /// Borrow the raw handle without transferring ownership or extending life.
    /// Unsafe native users must finish async work and all entries before owned
    /// Drop/static reuse; no safe conversion to the legacy async wrapper exists.
    #[inline]
    pub fn raw(&self) -> bindings::ove_i2c_t {
        self.handle
    }

    /// Write data to an I2C device.
    pub fn write(&mut self, addr: u16, data: &[u8], timeout: core::time::Duration) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_write(
                self.handle,
                addr,
                data.as_ptr().cast(),
                data.len(),
                crate::duration::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Read data from an I2C device.
    pub fn read(&mut self, addr: u16, buf: &mut [u8], timeout: core::time::Duration) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_read(
                self.handle,
                addr,
                buf.as_mut_ptr().cast(),
                buf.len(),
                crate::duration::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Combined write-then-read with I2C repeated start. An empty `tx`
    /// makes it a plain [`read`](Self::read).
    pub fn write_read(
        &mut self,
        addr: u16,
        tx: &[u8],
        rx: &mut [u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_write_read(
                self.handle,
                addr,
                tx.as_ptr().cast(),
                tx.len(),
                rx.as_mut_ptr().cast(),
                rx.len(),
                crate::duration::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Write to a single-byte-addressed register.
    pub fn reg_write(
        &mut self,
        addr: u16,
        reg: u8,
        data: &[u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_reg_write(
                self.handle,
                addr,
                reg,
                data.as_ptr().cast(),
                data.len(),
                crate::duration::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Read from a single-byte-addressed register.
    pub fn reg_read(
        &mut self,
        addr: u16,
        reg: u8,
        buf: &mut [u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_reg_read(
                self.handle,
                addr,
                reg,
                buf.as_mut_ptr().cast(),
                buf.len(),
                crate::duration::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Probe for a device at the given address (zero-length write, check ACK).
    pub fn probe(&mut self, addr: u16, timeout: core::time::Duration) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_probe(self.handle, addr, crate::duration::dur_to_ns(timeout))
        };
        Error::from_code(rc)
    }
}

impl Drop for I2c {
    fn drop(&mut self) {
        match &self.ownership {
            #[cfg(not(zero_heap))]
            Ownership::Heap => {
                // SAFETY: only successful native create establishes this owner;
                // safe synchronous calls retain no operation after return.
                unsafe { bindings::ove_i2c_destroy(self.handle) };
            }
            Ownership::Static(storage) => {
                // SAFETY: this owner holds the successful static claim. Raw
                // escapes must be quiescent; finish cleanup before reuse.
                unsafe { bindings::ove_i2c_deinit(self.handle) };
                storage.claimed.store(false, Ordering::Release);
            }
            Ownership::Borrowed => {}
        }
    }
}
