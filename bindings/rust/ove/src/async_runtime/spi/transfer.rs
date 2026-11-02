// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};

use super::state::{self, Lease, Outcome};
use super::{ChipSelect, Controller, Result, Word};

enum Buffers<'a, W: Word> {
    Duplex { tx: &'a [W], rx: &'a mut [W] },
    Read(&'a mut [W]),
    Write(&'a [W]),
    InPlace(&'a mut [W]),
}

/// One borrowed operation over stable owned native staging. No counted lease
/// lives in this Future across Pending; forgetting it cannot leak a lease.
pub struct Transfer<'a, W: Word> {
    owner: &'a mut Controller<W>,
    buffers: Buffers<'a, W>,
    cs: Option<ChipSelect>,
    generation: Option<usize>,
    done: Option<Result<()>>,
}

// Moving the Future moves only borrows/descriptions, never native backing.
impl<W: Word> Unpin for Transfer<'_, W> {}

impl<'a, W: Word> Transfer<'a, W> {
    fn new(owner: &'a mut Controller<W>, cs: Option<ChipSelect>, buffers: Buffers<'a, W>) -> Self {
        Self {
            owner,
            buffers,
            cs,
            generation: None,
            done: None,
        }
    }
    pub(super) fn duplex(
        owner: &'a mut Controller<W>,
        cs: Option<ChipSelect>,
        tx: &'a [W],
        rx: &'a mut [W],
    ) -> Self {
        Self::new(owner, cs, Buffers::Duplex { tx, rx })
    }
    pub(super) fn write(owner: &'a mut Controller<W>, cs: Option<ChipSelect>, tx: &'a [W]) -> Self {
        Self::new(owner, cs, Buffers::Write(tx))
    }
    pub(super) fn read(
        owner: &'a mut Controller<W>,
        cs: Option<ChipSelect>,
        rx: &'a mut [W],
    ) -> Self {
        Self::new(owner, cs, Buffers::Read(rx))
    }
    pub(super) fn in_place(
        owner: &'a mut Controller<W>,
        cs: Option<ChipSelect>,
        words: &'a mut [W],
    ) -> Self {
        Self::new(owner, cs, Buffers::InPlace(words))
    }

    fn input(&self) -> (*const W, usize, usize) {
        match &self.buffers {
            Buffers::Duplex { tx, rx } => (tx.as_ptr(), tx.len(), tx.len().max(rx.len())),
            Buffers::Write(tx) => (tx.as_ptr(), tx.len(), tx.len()),
            Buffers::Read(rx) => (core::ptr::null(), 0, rx.len()),
            Buffers::InPlace(words) => (words.as_ptr(), words.len(), words.len()),
        }
    }

    unsafe fn copy_back(&mut self, source: *const u8) {
        let destination = match &mut self.buffers {
            Buffers::Duplex { rx, .. } | Buffers::Read(rx) | Buffers::InPlace(rx) => &mut **rx,
            Buffers::Write(_) => return,
        };
        if !destination.is_empty() {
            // SAFETY: final callback published RX before Ready. The Consuming
            // phase excludes reuse until this live future's bounded copy ends.
            unsafe {
                core::ptr::copy_nonoverlapping(
                    source.cast::<W>(),
                    destination.as_mut_ptr(),
                    destination.len(),
                )
            };
        }
    }
}

impl<W: Word> Future for Transfer<'_, W> {
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
            let (tx, tx_words, words) = this.input();
            if words == 0 {
                this.done = Some(Ok(()));
                return Poll::Ready(Ok(()));
            }
            let waiter = context.waker().clone();
            match unsafe { state::begin(header, tx, tx_words, words, this.cs, waiter) } {
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
                unreachable!("accepted SPI result disappeared")
            }
        }
    }
}

impl<W: Word> Drop for Transfer<'_, W> {
    fn drop(&mut self) {
        if let Some(generation) = self.generation {
            // No cancel/refcount decrement: stable native ownership outlives
            // either Drop or forget. Only the matching waiter is detached.
            unsafe { state::detach(self.owner.header, generation) };
        }
    }
}

/// Wait for and consume an outstanding orphan without retaining application RX.
pub struct Flush<'a, W: Word> {
    owner: &'a mut Controller<W>,
    generation: Option<usize>,
    done: Option<Result<()>>,
}

impl<W: Word> Unpin for Flush<'_, W> {}

impl<'a, W: Word> Flush<'a, W> {
    pub(super) fn new(owner: &'a mut Controller<W>) -> Self {
        Self {
            owner,
            generation: None,
            done: None,
        }
    }
}

impl<W: Word> Future for Flush<'_, W> {
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

impl<W: Word> Drop for Flush<'_, W> {
    fn drop(&mut self) {
        if let Some(generation) = self.generation {
            unsafe { state::detach(self.owner.header, generation) };
        }
    }
}
