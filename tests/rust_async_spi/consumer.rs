// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::AsyncSpiStorage;
use ove::spi::{ChipSelect, Config};

static BYTES: AsyncSpiStorage<128> = AsyncSpiStorage::new();
static WORDS: AsyncSpiStorage<128, u16> = AsyncSpiStorage::new();

/// Ordinary application construction and trait use through public typed APIs.
#[inline(never)]
pub async fn public_async_spi_consumer() -> ove::Result<()> {
    use embedded_hal_async::spi::SpiBus;
    core::hint::black_box(&ASYNC_SPI_OWNER_SIZE);
    core::hint::black_box(&ASYNC_SPI_STORAGE_EMPTY_SIZE);
    core::hint::black_box(&ASYNC_SPI_STORAGE_BYTE_SIZE);
    core::hint::black_box(&ASYNC_SPI_STORAGE_WORD_SIZE);
    core::hint::black_box(&ASYNC_SPI_TRANSFER_SIZE);
    let config = Config::default();
    let cs = ChipSelect::new(0, 4, true);
    let mut bytes = BYTES.init(config)?;
    let mut read = [0; 4];
    bytes.transfer(Some(&cs), &[1, 2], &mut read).await?;
    SpiBus::transfer(&mut bytes, &mut read[..1], &[3, 4]).await?;
    SpiBus::read(&mut bytes, &mut read).await?;
    SpiBus::write(&mut bytes, &[5, 6]).await?;
    SpiBus::transfer_in_place(&mut bytes, &mut read).await?;
    SpiBus::flush(&mut bytes).await?;
    drop(bytes);
    let mut words = WORDS.init(config)?;
    let mut read = [0u16; 4];
    words
        .transfer(Some(&cs), &[0x1234, 0x5678], &mut read)
        .await?;
    SpiBus::transfer(&mut words, &mut read[..1], &[0xabcdu16, 0xef01]).await?;
    SpiBus::read(&mut words, &mut read).await?;
    SpiBus::write(&mut words, &[0x1234u16]).await?;
    SpiBus::transfer_in_place(&mut words, &mut read).await?;
    SpiBus::flush(&mut words).await?;
    drop(words);
    #[cfg(not(zero_heap))]
    {
        let mut bytes = ove::AsyncSpi::new(config)?;
        bytes.write(None, &[1, 2]).await?;
        bytes.flush().await?;
        let mut words = ove::AsyncSpi16::new(config)?;
        words.write(None, &[0x1234]).await?;
        words.flush().await?;
    }
    Ok(())
}

pub static ASYNC_SPI_OWNER_SIZE: [u8; size_of::<ove::AsyncSpi>()] = [0; size_of::<ove::AsyncSpi>()];
pub static ASYNC_SPI_STORAGE_EMPTY_SIZE: [u8; size_of::<AsyncSpiStorage<0>>()] =
    [0; size_of::<AsyncSpiStorage<0>>()];
pub static ASYNC_SPI_STORAGE_BYTE_SIZE: [u8; size_of::<AsyncSpiStorage<128>>()] =
    [0; size_of::<AsyncSpiStorage<128>>()];
pub static ASYNC_SPI_STORAGE_WORD_SIZE: [u8; size_of::<AsyncSpiStorage<128, u16>>()] =
    [0; size_of::<AsyncSpiStorage<128, u16>>()];
pub static ASYNC_SPI_TRANSFER_SIZE: [u8;
    size_of::<ove::async_runtime::spi::Transfer<'static, u8>>()] =
    [0; size_of::<ove::async_runtime::spi::Transfer<'static, u8>>()];
