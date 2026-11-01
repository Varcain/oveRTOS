// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use core::time::Duration;
use ove::spi::{ChipSelect, Config, Transfer};
use ove::{Spi16, SpiStorage};

static BYTES: SpiStorage = SpiStorage::new();
static WORDS: SpiStorage<u16> = SpiStorage::new();

#[inline(never)]
#[unsafe(no_mangle)]
pub fn spi_binding_consumer() -> ove::Result<()> {
    use embedded_hal::spi::SpiBus;
    core::hint::black_box(&SPI_BINDING_OWNER_SIZE);
    core::hint::black_box(&SPI_BINDING_STORAGE_SIZE);
    core::hint::black_box(&SPI_BINDING_DESCRIPTOR_SIZE);
    let config = Config::default();
    let cs = ChipSelect::new(0, 4, true);
    let timeout = Duration::from_millis(20);
    let mut bytes = BYTES.init(config)?;
    let mut rx = [0u8; 3];
    spi_binding_direct(&mut bytes, &[1, 2], &mut rx)?;
    let mut descriptors = [Transfer::duplex(&[3, 4], &mut rx), Transfer::write(&[5])];
    spi_binding_sequence(&mut bytes, &mut descriptors)?;
    SpiBus::transfer(&mut bytes, &mut rx[..1], &[6, 7])?;
    SpiBus::transfer_in_place(&mut bytes, &mut rx)?;
    SpiBus::flush(&mut bytes)?;
    let mut words: Spi16 = WORDS.init(config)?;
    let mut rx = [0u16; 3];
    words.transfer(Some(&cs), &[0x1234, 0x5678], &mut rx, timeout)?;
    words.transfer_seq(None, &mut [Transfer::in_place(&mut rx)], timeout)?;
    SpiBus::transfer(&mut words, &mut rx[..1], &[0xabcd, 0xeffe])?;
    SpiBus::transfer_in_place(&mut words, &mut rx)?;
    SpiBus::flush(&mut words)?;
    #[cfg(not(zero_heap))]
    {
        let mut heap = ove::Spi::new(config)?;
        heap.write(None, &[9], timeout)?;
        let mut heap = Spi16::new(config)?;
        heap.write(None, &[0x1234], timeout)?;
    }
    Ok(())
}

#[inline(never)]
#[unsafe(no_mangle)]
pub fn spi_binding_direct(spi: &mut ove::Spi, tx: &[u8], rx: &mut [u8]) -> ove::Result<()> {
    spi.transfer(None, tx, rx, Duration::from_millis(20))
}

#[inline(never)]
#[unsafe(no_mangle)]
pub fn spi_binding_sequence(spi: &mut ove::Spi, entries: &mut [Transfer<'_>]) -> ove::Result<()> {
    spi.transfer_seq(None, entries, Duration::from_millis(20))
}

#[unsafe(no_mangle)]
pub static SPI_BINDING_OWNER_SIZE: [u8; size_of::<ove::Spi>()] = [0; size_of::<ove::Spi>()];
#[unsafe(no_mangle)]
pub static SPI_BINDING_STORAGE_SIZE: [u8; size_of::<SpiStorage>()] = [0; size_of::<SpiStorage>()];
#[unsafe(no_mangle)]
pub static SPI_BINDING_DESCRIPTOR_SIZE: [u8; size_of::<Transfer<'static>>()] =
    [0; size_of::<Transfer<'static>>()];
