// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! embassy-net driver for the STM32F7 ETH MAC + LAN8742A PHY.
//!
//! Same shape as [`super::qemu_shm::QemuShmDriver`] — the differences
//! are all behind the C entry points (`ove_stm32f7_eth_async_*` vs
//! `ove_qemu_net_async_*`). For now the driver polls; spawn a poller
//! task alongside the embassy-net Runner:
//!
//! ```ignore
//! #[embassy_executor::task]
//! async fn poll_task() {
//!     loop {
//!         embassy_time::Timer::after_millis(2).await;
//!         ove::async_net::stm32f7_eth::wake_poll();
//!     }
//! }
//! ```
//!
//! ISR-backed wake (via `HAL_ETH_IRQHandler`) is a future optimisation
//! — the STM32F7 ETH IRQ exists but stock oveRTOS doesn't enable it
//! (the lwIP backend also polls).

use ::core::ffi::c_void;
use ::core::sync::atomic::{AtomicBool, Ordering};
use ::core::task::Context;

use embassy_net_driver::{Capabilities, Driver, HardwareAddress, LinkState, RxToken, TxToken};
use embassy_sync::waitqueue::AtomicWaker;

/// Max Ethernet frame size (Ethernet header + IP MTU + CRC slack).
pub const MTU: usize = 1518;

unsafe extern "C" {
    fn ove_stm32f7_eth_async_init(mac: *const u8) -> ::core::ffi::c_int;
    fn ove_stm32f7_eth_async_tx(frame: *const c_void, len: u32) -> ::core::ffi::c_int;
    fn ove_stm32f7_eth_async_rx(
        buf: *mut c_void,
        buf_size: u32,
        out_len: *mut u32,
    ) -> ::core::ffi::c_int;
    fn ove_stm32f7_eth_async_link_up() -> ::core::ffi::c_int;
    fn ove_stm32f7_eth_async_set_notify(
        cb: Option<unsafe extern "C" fn(*mut c_void)>,
        user_data: *mut c_void,
    );
}

/// ISR trampoline: the ETH IRQ → HAL_ETH_RxCpltCallback fires this,
/// which wakes the embassy-net runner. We pass `&RX_WAKER` as user_data;
/// dereferencing is sound because it's a 'static.
unsafe extern "C" fn isr_notify(_ud: *mut c_void) {
    RX_WAKER.wake();
}

static RX_WAKER: AtomicWaker = AtomicWaker::new();
static INITED: AtomicBool = AtomicBool::new(false);

/// Wake the embassy-net runner to re-poll the ETH MAC.
///
/// Since the driver wakes itself from `ETH_IRQHandler` on RX/TX
/// completion, calling this is only useful as a slow link-state poll
/// (every few hundred ms — enough to notice cable unplugs). It is
/// idempotent and safe from any context.
pub fn wake_poll() {
    RX_WAKER.wake();
}

/// STM32F7 ETH MAC driver.
pub struct Stm32f7EthDriver {
    mac: [u8; 6],
}

impl Stm32f7EthDriver {
    /// Initialise the MAC + PHY and return a driver handle.
    /// Blocks for up to ~5 s waiting for PHY autoneg.
    pub fn new(mac: [u8; 6]) -> Self {
        // SAFETY: C init touches static MAC state only; idempotent on
        // re-init (resets ring positions).
        let _ = unsafe { ove_stm32f7_eth_async_init(mac.as_ptr()) };
        // SAFETY: passing a function pointer + null user_data (the
        // trampoline reads the 'static RX_WAKER directly).
        unsafe {
            ove_stm32f7_eth_async_set_notify(Some(isr_notify), ::core::ptr::null_mut());
        }
        INITED.store(true, Ordering::Release);
        Self { mac }
    }
}

pub struct Stm32f7EthRx {
    buf: [u8; MTU],
    len: usize,
}

impl RxToken for Stm32f7EthRx {
    fn consume<R, F>(mut self, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        f(&mut self.buf[..self.len])
    }
}

pub struct Stm32f7EthTx;

impl TxToken for Stm32f7EthTx {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let mut buf = [0u8; MTU];
        let n = len.min(MTU);
        let result = f(&mut buf[..n]);
        // SAFETY: pointer + length are valid; HAL_ETH_Transmit copies
        // the data into the descriptor before returning.
        unsafe {
            ove_stm32f7_eth_async_tx(buf.as_ptr() as *const c_void, n as u32);
        }
        result
    }
}

impl Driver for Stm32f7EthDriver {
    type RxToken<'a>
        = Stm32f7EthRx
    where
        Self: 'a;
    type TxToken<'a>
        = Stm32f7EthTx
    where
        Self: 'a;

    fn receive(&mut self, cx: &mut Context<'_>) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let mut rx = Stm32f7EthRx {
            buf: [0; MTU],
            len: 0,
        };
        let mut out_len: u32 = 0;
        let rc = unsafe {
            ove_stm32f7_eth_async_rx(
                rx.buf.as_mut_ptr() as *mut c_void,
                MTU as u32,
                &mut out_len as *mut u32,
            )
        };
        if rc == 0 && out_len > 0 {
            rx.len = out_len as usize;
            Some((rx, Stm32f7EthTx))
        } else {
            RX_WAKER.register(cx.waker());
            None
        }
    }

    fn transmit(&mut self, _cx: &mut Context<'_>) -> Option<Self::TxToken<'_>> {
        Some(Stm32f7EthTx)
    }

    fn link_state(&mut self, _cx: &mut Context<'_>) -> LinkState {
        // SAFETY: pure PHY register read.
        let up = unsafe { ove_stm32f7_eth_async_link_up() };
        if up != 0 {
            LinkState::Up
        } else {
            LinkState::Down
        }
    }

    fn capabilities(&self) -> Capabilities {
        let mut caps = Capabilities::default();
        caps.max_transmission_unit = 1514;
        caps
    }

    fn hardware_address(&self) -> HardwareAddress {
        HardwareAddress::Ethernet(self.mac)
    }
}
