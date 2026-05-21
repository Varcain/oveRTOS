// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! embassy-net driver for the QEMU MPS2-AN500 shared-memory Ethernet
//! transport.
//!
//! Frames flow guest ↔ host through `/dev/shm/ove-net` via ARM
//! semihosting; the host-side bridge (`config/scripts/qemu-net-bridge.py`)
//! converts to/from a Linux TAP interface.
//!
//! The driver polls (no interrupt) — receive() returns frames already
//! delivered to the SHM ring and registers the embassy-net waker so a
//! background timer task can re-poll. For a typical app, spawn a small
//! poller alongside the embassy-net Runner:
//!
//! ```ignore
//! #[embassy_executor::task]
//! async fn poll_task() {
//!     loop {
//!         embassy_time::Timer::after_millis(10).await;
//!         ove::async_net::qemu_shm::wake_poll();
//!     }
//! }
//! ```

use ::core::ffi::{c_int, c_void};
use ::core::sync::atomic::{AtomicBool, Ordering};
use ::core::task::Context;

use embassy_net_driver::{Capabilities, Driver, HardwareAddress, LinkState, RxToken, TxToken};
use embassy_sync::waitqueue::AtomicWaker;

/// Max Ethernet frame size — matches `NET_SHM_MTU` in the C header.
pub const MTU: usize = 1518;

// FFI surface from boards/qemu-mps2-an500/src/qemu_net_async.c.
// Declared inline here rather than going through bindings_stub.rs
// because the board crate is the consumer and the symbols are
// always paired with the `async-net-qemu-shm` Cargo feature.
unsafe extern "C" {
    fn ove_qemu_net_async_init(mac: *const u8) -> c_int;
    fn ove_qemu_net_async_tx(frame: *const c_void, len: u32) -> c_int;
    fn ove_qemu_net_async_rx(buf: *mut c_void, buf_size: u32, out_len: *mut u32) -> c_int;
    fn ove_qemu_net_async_link_up() -> c_int;
}

/// Waker the periodic poller signals so the embassy-net runner re-checks
/// the RX ring. Single-waker is sufficient because embassy-net runs a
/// single Runner task per Driver.
static RX_WAKER: AtomicWaker = AtomicWaker::new();
static INITED: AtomicBool = AtomicBool::new(false);

/// Wake the embassy-net runner to re-poll the RX ring. Call this
/// periodically (e.g. from a 10 ms timer task) to drive frame delivery.
pub fn wake_poll() {
    RX_WAKER.wake();
}

/// Driver state — owns a single RX frame buffer to keep storage small.
/// The C side serialises ring access; a single in-flight frame at a
/// time is fine for the SHM transport's throughput envelope.
pub struct QemuShmDriver {
    mac: [u8; 6],
}

impl QemuShmDriver {
    /// Construct the driver and announce ourselves to the host bridge.
    /// Safe to call once; subsequent calls reset ring positions.
    pub fn new(mac: [u8; 6]) -> Self {
        // SAFETY: ove_qemu_net_async_init only touches static C state.
        let _ = unsafe { ove_qemu_net_async_init(mac.as_ptr()) };
        INITED.store(true, Ordering::Release);
        Self { mac }
    }
}

/// RX token — wraps a stack-local frame buffer the runner reads from.
pub struct QemuShmRx {
    buf: [u8; MTU],
    len: usize,
}

impl RxToken for QemuShmRx {
    fn consume<R, F>(mut self, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        f(&mut self.buf[..self.len])
    }
}

/// TX token — owns a stack-local frame buffer the runner fills, then
/// the consume() closure flushes to the SHM ring.
pub struct QemuShmTx;

impl TxToken for QemuShmTx {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let mut buf = [0u8; MTU];
        let n = len.min(MTU);
        let result = f(&mut buf[..n]);
        // SAFETY: pointer + length are valid; the C side validates len.
        unsafe {
            ove_qemu_net_async_tx(buf.as_ptr() as *const c_void, n as u32);
        }
        result
    }
}

impl Driver for QemuShmDriver {
    type RxToken<'a>
        = QemuShmRx
    where
        Self: 'a;
    type TxToken<'a>
        = QemuShmTx
    where
        Self: 'a;

    fn receive(&mut self, cx: &mut Context<'_>) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let mut rx = QemuShmRx {
            buf: [0; MTU],
            len: 0,
        };
        let mut out_len: u32 = 0;
        // SAFETY: buf and out_len are owned by us, valid for the call.
        let rc = unsafe {
            ove_qemu_net_async_rx(
                rx.buf.as_mut_ptr() as *mut c_void,
                MTU as u32,
                &mut out_len as *mut u32,
            )
        };
        if rc == 0 && out_len > 0 {
            rx.len = out_len as usize;
            Some((rx, QemuShmTx))
        } else {
            RX_WAKER.register(cx.waker());
            None
        }
    }

    fn transmit(&mut self, _cx: &mut Context<'_>) -> Option<Self::TxToken<'_>> {
        // SHM TX is always available (we drop on ring-full inside consume).
        Some(QemuShmTx)
    }

    fn link_state(&mut self, _cx: &mut Context<'_>) -> LinkState {
        // SAFETY: pure read of a C int.
        let up = unsafe { ove_qemu_net_async_link_up() };
        if up != 0 {
            LinkState::Up
        } else {
            LinkState::Down
        }
    }

    fn capabilities(&self) -> Capabilities {
        // Medium is implied by hardware_address() returning Ethernet.
        // MTU is Ethernet-frame-size (per embassy-net-driver 0.2 docs).
        let mut caps = Capabilities::default();
        caps.max_transmission_unit = 1514; // 1500 IP MTU + 14 Ethernet header
        caps
    }

    fn hardware_address(&self) -> HardwareAddress {
        HardwareAddress::Ethernet(self.mac)
    }
}
