// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Completion slot shared by the async SPI and I2C wrappers: each in-flight
//! transfer parks its waker here and the C completion callback stores the
//! result and wakes it. Built whenever either bus is configured, so I2C does
//! not depend on SPI being enabled.

use ::core::sync::atomic::{AtomicI32, Ordering};

use embassy_sync::waitqueue::AtomicWaker;

/// Per-transfer slot — wakes the caller and stores the completion result.
/// `UnsafeCell` rather than `Mutex` because we use atomics for the
/// state field, and `AtomicWaker` is itself interior-mut-safe.
pub struct DmaSlot {
    waker: AtomicWaker,
    result: AtomicI32,
}

impl DmaSlot {
    /// Sentinel marking a transfer still in flight. `i32::MIN` is well
    /// outside the negative-OVE-error range (which is -1 to -20-ish).
    pub const PENDING: i32 = i32::MIN;

    pub const fn new() -> Self {
        Self {
            waker: AtomicWaker::new(),
            result: AtomicI32::new(Self::PENDING),
        }
    }

    pub fn reset(&self) {
        self.result.store(Self::PENDING, Ordering::Release);
    }

    pub fn result_store(&self, value: i32) {
        self.result.store(value, Ordering::Release);
    }

    pub fn result_load(&self) -> i32 {
        self.result.load(Ordering::Acquire)
    }

    pub fn register(&self, w: &::core::task::Waker) {
        self.waker.register(w);
    }

    pub fn wake(&self) {
        self.waker.wake();
    }
}
