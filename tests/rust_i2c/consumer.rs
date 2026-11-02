// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use core::time::Duration;
use ove::{I2c, I2cStorage, i2c::Config};

static BUS: I2cStorage = I2cStorage::new();

#[inline(never)]
#[unsafe(no_mangle)]
pub fn i2c_binding_consumer() -> ove::Result<()> {
    use embedded_hal::i2c::{I2c as _, Operation};
    core::hint::black_box(&I2C_BINDING_OWNER_SIZE);
    core::hint::black_box(&I2C_BINDING_STORAGE_SIZE);
    let timeout = Duration::from_millis(20);
    let mut bus = BUS.init(Config::default())?;
    let mut rx = [0; 7];
    i2c_binding_direct(&mut bus, &[1, 2, 3], &mut rx)?;
    bus.reg_write(0x42, 0x10, &[1, 2], timeout)?;
    bus.reg_read(0x42, 0x10, &mut rx, timeout)?;
    bus.probe(0x42, timeout)?;
    bus.transaction(
        0x42,
        &mut [Operation::Write(&[1, 2]), Operation::Read(&mut rx)],
    )?;
    embedded_hal::i2c::I2c::write_read(&mut bus, 0x42, &[3], &mut rx)?;
    Ok(())
}

#[inline(never)]
#[unsafe(no_mangle)]
pub fn i2c_binding_direct(bus: &mut I2c, tx: &[u8], rx: &mut [u8]) -> ove::Result<()> {
    bus.write_read(0x42, tx, rx, Duration::from_millis(20))
}

#[unsafe(no_mangle)]
pub static I2C_BINDING_OWNER_SIZE: [u8; size_of::<I2c>()] = [0; size_of::<I2c>()];
#[unsafe(no_mangle)]
pub static I2C_BINDING_STORAGE_SIZE: [u8; size_of::<I2cStorage>()] = [0; size_of::<I2cStorage>()];
