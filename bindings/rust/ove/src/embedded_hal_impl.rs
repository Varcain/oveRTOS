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
                Error::BusError => i2c::ErrorKind::Bus,
                // BusBusy lands here: it means the controller or bus is busy
                // (a transfer in flight, or the bus held), not lost
                // arbitration (which oveRTOS reports as BusError), and
                // `embedded-hal` 1.0 has no "busy" kind.
                _ => i2c::ErrorKind::Other,
            }
        }
    }

    impl i2c::ErrorType for I2c {
        type Error = Error;
    }

    impl i2c::I2c for I2c {
        fn read(&mut self, address: u8, read: &mut [u8]) -> Result<(), Self::Error> {
            self.transaction(address, &mut [i2c::Operation::Read(read)])
        }
        fn write(&mut self, address: u8, write: &[u8]) -> Result<(), Self::Error> {
            self.transaction(address, &mut [i2c::Operation::Write(write)])
        }
        fn write_read(
            &mut self,
            address: u8,
            write: &[u8],
            read: &mut [u8],
        ) -> Result<(), Self::Error> {
            self.transaction(
                address,
                &mut [i2c::Operation::Write(write), i2c::Operation::Read(read)],
            )
        }
        fn transaction(
            &mut self,
            address: u8,
            operations: &mut [i2c::Operation<'_>],
        ) -> Result<(), Self::Error> {
            if address > 0x7f {
                return Err(Error::InvalidParam);
            }
            let timeout = core::time::Duration::MAX;
            // Match the whole shape before dispatch. Separate native calls
            // would introduce STOP intervals forbidden by the trait contract.
            match operations {
                [] => Ok(()),
                [i2c::Operation::Write(write)] => I2c::write(self, address.into(), write, timeout),
                [i2c::Operation::Read(read)] => {
                    if read.is_empty() {
                        return Err(Error::InvalidParam);
                    }
                    I2c::read(self, address.into(), read, timeout)
                }
                [i2c::Operation::Write(write), i2c::Operation::Read(read)] => {
                    if write.is_empty() {
                        return Err(Error::NotSupported);
                    }
                    if read.is_empty() {
                        return Err(Error::InvalidParam);
                    }
                    I2c::write_read(self, address.into(), write, read, timeout)
                }
                _ => Err(Error::NotSupported),
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SPI
// ---------------------------------------------------------------------------

#[cfg(has_spi)]
mod spi_impl {
    use super::*;
    use crate::spi::{Controller, Word};
    use embedded_hal::spi;

    impl spi::Error for Error {
        fn kind(&self) -> spi::ErrorKind {
            spi::ErrorKind::Other
        }
    }

    impl<W: Word> spi::ErrorType for Controller<W> {
        type Error = Error;
    }

    impl<W: Word> spi::SpiBus<W> for Controller<W> {
        fn read(&mut self, words: &mut [W]) -> Result<(), Self::Error> {
            Controller::read(
                self,
                None,
                words,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn write(&mut self, words: &[W]) -> Result<(), Self::Error> {
            Controller::write(
                self,
                None,
                words,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn transfer(&mut self, read: &mut [W], write: &[W]) -> Result<(), Self::Error> {
            Controller::transfer(
                self,
                None,
                write,
                read,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn transfer_in_place(&mut self, words: &mut [W]) -> Result<(), Self::Error> {
            Controller::transfer_in_place(
                self,
                None,
                words,
                core::time::Duration::from_secs(u64::MAX / 2),
            )
        }

        fn flush(&mut self) -> Result<(), Self::Error> {
            // This controller's synchronous calls have returned. Unrelated
            // native/async users still require external bus coordination.
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
