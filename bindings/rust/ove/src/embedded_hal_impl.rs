// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! `embedded-hal` 1.0 sync trait impls.
//!
//! Compiled only when the `embedded-hal` Cargo feature is enabled.
//! Each section is further gated by the substrate `has_*` cfg for the
//! corresponding peripheral — a target build with I2C compiled out
//! omits the I2C impls automatically, same for SPI / GPIO / TIME.
//!
//! Provides the ecosystem-compatible trait surface for
//! `ove::I2c` / `ove::Spi` / `ove::gpio::OutputPin` / `ove::gpio::InputPin`
//! / `ove::Delay` so that any `embedded-hal`-aware crate.io driver
//! (BME280, MPU6050, SX1276, …) composes with oveRTOS peripherals at
//! zero per-call cost (monomorphisation inlines the wrappers).
//!
//! Error mapping into the `embedded_hal::*::ErrorKind` enums is best-
//! effort — the substrate's `OVE_ERR_*` taxonomy is coarser than
//! `embedded-hal`'s.  Unmappable variants fall back to `ErrorKind::Other`.

use crate::error::Error;

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------

#[cfg(has_i2c)]
mod i2c_impl {
    use super::*;
    use crate::I2c;
    use embedded_hal::i2c;

    impl i2c::Error for Error {
        fn kind(&self) -> i2c::ErrorKind {
            match self {
                Error::BusNack => i2c::ErrorKind::NoAcknowledge(i2c::NoAcknowledgeSource::Unknown),
                // `embedded-hal` 1.0 has no "busy" ErrorKind; ArbitrationLoss
                // is the closest retryable bus-contention signal, so drivers
                // that back off on arbitration loss also back off on a busy bus.
                Error::BusBusy => i2c::ErrorKind::ArbitrationLoss,
                Error::BusError => i2c::ErrorKind::Bus,
                _ => i2c::ErrorKind::Other,
            }
        }
    }

    impl i2c::ErrorType for I2c {
        type Error = Error;
    }

    impl i2c::I2c for I2c {
        fn read(&mut self, address: u8, read: &mut [u8]) -> Result<(), Self::Error> {
            I2c::read(
                self,
                address as u16,
                read,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn write(&mut self, address: u8, write: &[u8]) -> Result<(), Self::Error> {
            I2c::write(
                self,
                address as u16,
                write,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn write_read(
            &mut self,
            address: u8,
            write: &[u8],
            read: &mut [u8],
        ) -> Result<(), Self::Error> {
            I2c::write_read(
                self,
                address as u16,
                write,
                read,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn transaction(
            &mut self,
            address: u8,
            operations: &mut [i2c::Operation<'_>],
        ) -> Result<(), Self::Error> {
            // The substrate doesn't expose explicit START/STOP control,
            // so atomicity across a heterogeneous sequence isn't
            // guaranteed.  We do fuse Write+Read into `write_read`
            // (which DOES emit a repeated START) for the common
            // "register address then read" pattern.
            let mut i = 0;
            while i < operations.len() {
                if i + 1 < operations.len() {
                    let (left, right) = operations.split_at_mut(i + 1);
                    if let (i2c::Operation::Write(w), i2c::Operation::Read(r)) =
                        (&left[i], &mut right[0])
                    {
                        self.write_read(address, w, r)?;
                        i += 2;
                        continue;
                    }
                }
                match &mut operations[i] {
                    i2c::Operation::Read(buf) => self.read(address, buf)?,
                    i2c::Operation::Write(buf) => self.write(address, buf)?,
                }
                i += 1;
            }
            Ok(())
        }
    }
}

// ---------------------------------------------------------------------------
// SPI
// ---------------------------------------------------------------------------

#[cfg(has_spi)]
mod spi_impl {
    use super::*;
    use crate::Spi;
    use crate::bindings;
    use embedded_hal::spi;

    impl spi::Error for Error {
        fn kind(&self) -> spi::ErrorKind {
            spi::ErrorKind::Other
        }
    }

    impl spi::ErrorType for Spi {
        type Error = Error;
    }

    impl spi::SpiBus<u8> for Spi {
        fn read(&mut self, words: &mut [u8]) -> Result<(), Self::Error> {
            Spi::read(
                self,
                None,
                words,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn write(&mut self, words: &[u8]) -> Result<(), Self::Error> {
            Spi::write(
                self,
                None,
                words,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn transfer(&mut self, read: &mut [u8], write: &[u8]) -> Result<(), Self::Error> {
            Spi::transfer(
                self,
                None,
                write,
                read,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn transfer_in_place(&mut self, words: &mut [u8]) -> Result<(), Self::Error> {
            // Substrate accepts `tx_ptr == rx_ptr` — SPI hardware
            // shifts bits through the same memory location.  No tmp
            // buffer needed.
            let len = words.len();
            let p = words.as_mut_ptr();
            let rc = unsafe {
                bindings::ove_spi_transfer(
                    self.raw(),
                    core::ptr::null(),
                    p as *const _,
                    p as *mut _,
                    len,
                    u64::MAX,
                )
            };
            Error::from_code(rc)
        }

        fn flush(&mut self) -> Result<(), Self::Error> {
            // Substrate transfers are synchronous — completion implies
            // bus quiescence.
            Ok(())
        }
    }
}

// ---------------------------------------------------------------------------
// Digital GPIO
// ---------------------------------------------------------------------------

#[cfg(has_gpio)]
mod gpio_impl {
    use super::*;
    use crate::gpio::{InputPin, OutputPin};
    use embedded_hal::digital;

    impl digital::Error for Error {
        fn kind(&self) -> digital::ErrorKind {
            digital::ErrorKind::Other
        }
    }

    impl digital::ErrorType for OutputPin {
        type Error = Error;
    }

    impl digital::OutputPin for OutputPin {
        fn set_low(&mut self) -> Result<(), Self::Error> {
            OutputPin::set_low(self)
        }

        fn set_high(&mut self) -> Result<(), Self::Error> {
            OutputPin::set_high(self)
        }
    }

    impl digital::ErrorType for InputPin {
        type Error = Error;
    }

    impl digital::InputPin for InputPin {
        fn is_high(&mut self) -> Result<bool, Self::Error> {
            InputPin::is_high(self)
        }

        fn is_low(&mut self) -> Result<bool, Self::Error> {
            InputPin::is_low(self)
        }
    }
}

// ---------------------------------------------------------------------------
// Delay
// ---------------------------------------------------------------------------

#[cfg(has_time)]
mod delay_impl {
    use crate::time::{Delay, delay_ms, delay_us};
    use embedded_hal::delay::DelayNs;

    impl DelayNs for Delay {
        fn delay_ns(&mut self, ns: u32) {
            if ns == 0 {
                return;
            }
            // Round up to microsecond — substrate has no ns resolution.
            let us = ns.div_ceil(1_000);
            delay_us(us);
        }

        fn delay_us(&mut self, us: u32) {
            delay_us(us);
        }

        fn delay_ms(&mut self, ms: u32) {
            delay_ms(ms);
        }
    }
}
