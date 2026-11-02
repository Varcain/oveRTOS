// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::{AsyncI2cStorage, i2c::Config};
static BUS: AsyncI2cStorage<3, 7> = AsyncI2cStorage::new();
static READ_ONLY: AsyncI2cStorage<0, 7> = AsyncI2cStorage::new();
static WRITE_ONLY: AsyncI2cStorage<3, 0> = AsyncI2cStorage::new();

/// Ordinary awaited application operations through the safe public binding.
#[inline(never)]
pub async fn public_async_i2c_consumer() -> ove::Result<()> {
    use embedded_hal_async::i2c::{I2c, Operation};
    core::hint::black_box(&ASYNC_I2C_OWNER_SIZE);
    core::hint::black_box(&ASYNC_I2C_STORAGE_EMPTY_SIZE);
    core::hint::black_box(&ASYNC_I2C_STORAGE_SIZE);
    core::hint::black_box(&ASYNC_I2C_TRANSFER_SIZE);
    let config = Config::default();
    let mut bus = BUS.init(config)?;
    let mut rx = [0; 7];
    bus.write_read(0x42, &[1, 2, 3], &mut rx).await?;
    bus.write(0x42, &[1, 2, 3]).await?;
    bus.read(0x42, &mut rx).await?;
    I2c::write_read(&mut bus, 0x42, &[1, 2, 3], &mut rx).await?;
    I2c::write(&mut bus, 0x42, &[1, 2, 3]).await?;
    I2c::read(&mut bus, 0x42, &mut rx).await?;
    I2c::transaction(
        &mut bus,
        0x42,
        &mut [Operation::Write(&[1, 2, 3]), Operation::Read(&mut rx)],
    )
    .await?;
    bus.flush().await?;
    drop(bus);
    let mut read = READ_ONLY.init(config)?;
    read.read(0x42, &mut rx).await?;
    drop(read);
    let mut write = WRITE_ONLY.init(config)?;
    write.write(0x42, &[1, 2, 3]).await?;
    drop(write);
    #[cfg(not(zero_heap))]
    {
        let mut heap = ove::AsyncI2c::new(config)?;
        heap.write_read(0x42, &[1, 2, 3], &mut rx).await?;
        heap.flush().await?;
    }
    Ok(())
}
pub static ASYNC_I2C_OWNER_SIZE: [u8; size_of::<ove::AsyncI2c>()] = [0; size_of::<ove::AsyncI2c>()];
pub static ASYNC_I2C_STORAGE_EMPTY_SIZE: [u8; size_of::<AsyncI2cStorage<0>>()] =
    [0; size_of::<AsyncI2cStorage<0>>()];
pub static ASYNC_I2C_STORAGE_SIZE: [u8; size_of::<AsyncI2cStorage<3, 7>>()] =
    [0; size_of::<AsyncI2cStorage<3, 7>>()];
pub static ASYNC_I2C_TRANSFER_SIZE: [u8; size_of::<ove::async_runtime::i2c::Transfer<'static>>()] =
    [0; size_of::<ove::async_runtime::i2c::Transfer<'static>>()];
