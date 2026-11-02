// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};

use super::state::{self, Lease, Outcome};
use super::{AsyncI2c, Result};

enum Buffers<'a> {
    Duplex { tx: &'a [u8], rx: &'a mut [u8] },
    Read(&'a mut [u8]),
    Write(&'a [u8]),
}

/// One borrowed operation over stable owned native staging. No counted lease
/// lives in this Future across Pending; forgetting it cannot leak a lease.
pub struct Transfer<'a> {
    owner: &'a mut AsyncI2c,
    buffers: Buffers<'a>,
    address: u16,
    generation: Option<usize>,
    done: Option<Result<()>>,
}

// Moving the Future moves only borrows/descriptions, never native backing.
impl Unpin for Transfer<'_> {}

impl<'a> Transfer<'a> {
    fn new(owner: &'a mut AsyncI2c, address: u16, buffers: Buffers<'a>) -> Self {
        Self {
            owner,
            buffers,
            address,
            generation: None,
            done: None,
        }
    }
    pub(super) fn duplex(
        owner: &'a mut AsyncI2c,
        address: u16,
        tx: &'a [u8],
        rx: &'a mut [u8],
    ) -> Self {
        Self::new(owner, address, Buffers::Duplex { tx, rx })
    }
    pub(super) fn write(owner: &'a mut AsyncI2c, address: u16, tx: &'a [u8]) -> Self {
        Self::new(owner, address, Buffers::Write(tx))
    }
    pub(super) fn read(owner: &'a mut AsyncI2c, address: u16, rx: &'a mut [u8]) -> Self {
        Self::new(owner, address, Buffers::Read(rx))
    }
    fn input(&self) -> (*const u8, usize, usize) {
        match &self.buffers {
            Buffers::Duplex { tx, rx } => (tx.as_ptr(), tx.len(), rx.len()),
            Buffers::Write(tx) => (tx.as_ptr(), tx.len(), 0),
            Buffers::Read(rx) => (core::ptr::null(), 0, rx.len()),
        }
    }

    unsafe fn copy_back(&mut self, source: *const u8) {
        let destination = match &mut self.buffers {
            Buffers::Duplex { rx, .. } | Buffers::Read(rx) => &mut **rx,
            Buffers::Write(_) => return,
        };
        if !destination.is_empty() {
            // SAFETY: final callback published RX before Ready. The Consuming
            // phase excludes reuse until this live future's bounded copy ends.
            unsafe {
                core::ptr::copy_nonoverlapping(source, destination.as_mut_ptr(), destination.len())
            };
        }
    }
}

impl Future for Transfer<'_> {
    type Output = Result<()>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        if let Some(result) = this.done {
            return Poll::Ready(result);
        }
        let header = this.owner.header;
        // This guard exists only for this synchronous poll; no count depends
        // on the Future being dropped after it yields Pending.
        let _call = unsafe { Lease::acquire(header) };
        let waiter = if this.generation.is_none() {
            let (tx, tx_len, rx_len) = this.input();
            let waiter = context.waker().clone();
            match unsafe { state::begin(header, this.address, tx, tx_len, rx_len, waiter) } {
                Ok(generation) => this.generation = Some(generation),
                Err(error) => {
                    this.done = Some(Err(error));
                    return Poll::Ready(Err(error));
                }
            }
            // Begin already installed the first waiter before native entry.
            None
        } else {
            Some(context.waker().clone())
        };
        match unsafe { state::observe(header, this.generation, waiter) } {
            Outcome::Pending { replaced, .. } => {
                drop(replaced);
                Poll::Pending
            }
            Outcome::Ready {
                generation,
                result,
                rx,
                unused,
            } => {
                if result.is_ok() {
                    unsafe { this.copy_back(rx) };
                }
                unsafe { state::consumed(header, generation) };
                this.generation = None;
                this.done = Some(result);
                // RX and operation state are finished before arbitrary Waker
                // destruction; only the independent call lease remains.
                drop(unused);
                Poll::Ready(result)
            }
            Outcome::Idle { unused } => {
                drop(unused);
                unreachable!("accepted I2C result disappeared")
            }
        }
    }
}

impl Drop for Transfer<'_> {
    fn drop(&mut self) {
        if let Some(generation) = self.generation {
            // No cancel/refcount decrement: stable native ownership outlives
            // either Drop or forget. Only the matching waiter is detached.
            unsafe { state::detach(self.owner.header, generation) };
        }
    }
}

/// Wait for and consume an outstanding orphan without retaining application RX.
pub struct Flush<'a> {
    owner: &'a mut AsyncI2c,
    generation: Option<usize>,
    done: Option<Result<()>>,
}

impl Unpin for Flush<'_> {}

impl<'a> Flush<'a> {
    pub(super) fn new(owner: &'a mut AsyncI2c) -> Self {
        Self {
            owner,
            generation: None,
            done: None,
        }
    }
}

impl Future for Flush<'_> {
    type Output = Result<()>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        if let Some(result) = this.done {
            return Poll::Ready(result);
        }
        let header = this.owner.header;
        let _call = unsafe { Lease::acquire(header) };
        let waiter = context.waker().clone();
        match unsafe { state::observe(header, this.generation, Some(waiter)) } {
            Outcome::Pending {
                generation,
                replaced,
            } => {
                this.generation = Some(generation);
                drop(replaced);
                Poll::Pending
            }
            Outcome::Ready {
                generation,
                result,
                unused,
                ..
            } => {
                unsafe { state::consumed(header, generation) };
                this.generation = None;
                this.done = Some(result);
                drop(unused);
                Poll::Ready(result)
            }
            Outcome::Idle { unused } => {
                this.generation = None;
                this.done = Some(Ok(()));
                drop(unused);
                Poll::Ready(Ok(()))
            }
        }
    }
}

impl Drop for Flush<'_> {
    fn drop(&mut self) {
        if let Some(generation) = self.generation {
            unsafe { state::detach(self.owner.header, generation) };
        }
    }
}
