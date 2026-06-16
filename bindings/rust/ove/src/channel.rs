// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Crossbeam-style MPMC channel over [`crate::Queue`].
//!
//! [`Sender<T, N>`] and [`Receiver<T, N>`] are cloneable handles to a
//! shared backing [`crate::Queue`]. Multiple producers can hold their
//! own [`Sender`]; multiple consumers can hold their own [`Receiver`].
//! The semantics match `std::sync::mpsc` / `crossbeam::channel`:
//!
//! - `Sender::send` blocks until the queue accepts the item, or returns
//!   [`Error::NetClosed`] if all receivers have been dropped.
//! - `Receiver::recv` blocks until a producer sends, or returns
//!   [`Error::NetClosed`] if all senders have been dropped.
//! - `Sender::try_send` is the non-blocking send: it returns
//!   [`Error::Timeout`] when the queue is momentarily full, or
//!   [`Error::NetClosed`] once all receivers have been dropped.  (The
//!   underlying [`crate::Queue`] reports full as `Error::QueueFull`; the
//!   channel maps that to the above.)
//! - `Receiver::try_recv` is the non-blocking receive: it returns
//!   [`TryRecvError::Empty`] when the queue is momentarily empty but
//!   senders remain, or [`TryRecvError::Disconnected`] once the queue is
//!   empty *and* all senders have been dropped — matching
//!   `std::sync::mpsc::Receiver::try_recv`.
//!
//! Variants:
//!
//! - **Heap mode** ([`channel`]) — convenience constructor that
//!   allocates the queue + a refcount on the heap. Requires the
//!   `alloc` feature.
//! - **Static mode** ([`Sender::from_static`]) — wraps a caller-owned
//!   `&'static Queue<T, N>` for zero-heap builds; refcount lives in a
//!   companion `&'static AtomicUsize` you provide.
//!
//! Mirrors `zephyr::sync::channel::{Sender, Receiver}` but rides on
//! the existing `ove_queue_*` FFI so frames can bridge to C producers
//! / consumers (the C side just calls `ove_queue_send` / `_receive`
//! and stays oblivious of the Rust-side refcount).

use ::core::sync::atomic::{AtomicUsize, Ordering};

use crate::error::{Error, Result};
use crate::queue::Queue;

#[cfg(all(feature = "alloc", not(zero_heap)))]
extern crate alloc;

/// Error returned by [`Receiver::try_recv`].
///
/// Mirrors [`std::sync::mpsc::TryRecvError`]: distinguishes a channel
/// that is *momentarily* empty (an item may still arrive) from one that
/// is empty *and* permanently closed (every [`Sender`] has been dropped,
/// so no item ever will).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TryRecvError {
    /// The channel is currently empty, but senders are still alive — an
    /// item may become available later.
    Empty,
    /// The channel is empty *and* every [`Sender`] has been dropped, so
    /// no further item can ever arrive.
    Disconnected,
}

impl core::fmt::Display for TryRecvError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            TryRecvError::Empty => write!(f, "receiving on an empty channel"),
            TryRecvError::Disconnected => {
                write!(f, "receiving on an empty and disconnected channel")
            }
        }
    }
}

// `core::error::Error` is stable since Rust 1.81 (< our 1.85 MSRV) and is
// re-exported as `std::error::Error`, so this single impl covers both
// `no_std` and `std` consumers.
impl core::error::Error for TryRecvError {}

/// Shared refcount + queue handle for the heap-mode allocation.
#[cfg(all(feature = "alloc", not(zero_heap)))]
struct ChannelInner<T: Copy, const N: usize> {
    queue: Queue<T, N>,
    tx_count: AtomicUsize,
    rx_count: AtomicUsize,
}

/// Multi-producer half of an [`ove::channel`](self).
pub struct Sender<T: Copy + 'static, const N: usize> {
    state: SenderState<T, N>,
}

enum SenderState<T: Copy + 'static, const N: usize> {
    #[cfg(all(feature = "alloc", not(zero_heap)))]
    Heap(alloc::sync::Arc<ChannelInner<T, N>>),
    Static {
        queue: &'static Queue<T, N>,
        tx_count: &'static AtomicUsize,
        rx_count: &'static AtomicUsize,
    },
}

/// Multi-consumer half of an [`ove::channel`](self).
pub struct Receiver<T: Copy + 'static, const N: usize> {
    state: ReceiverState<T, N>,
}

enum ReceiverState<T: Copy + 'static, const N: usize> {
    #[cfg(all(feature = "alloc", not(zero_heap)))]
    Heap(alloc::sync::Arc<ChannelInner<T, N>>),
    Static {
        queue: &'static Queue<T, N>,
        tx_count: &'static AtomicUsize,
        rx_count: &'static AtomicUsize,
    },
}

/// Construct a heap-allocated channel with one initial sender + one
/// initial receiver. Clone either half to fan out.
///
/// Requires the `alloc` feature and a heap-mode build (i.e. not
/// `CONFIG_OVE_ZERO_HEAP=y`). Zero-heap callers must use
/// [`Sender::from_static`] + [`Receiver::from_static`] instead.
#[cfg(all(feature = "alloc", not(zero_heap)))]
pub fn channel<T: Copy + 'static, const N: usize>() -> Result<(Sender<T, N>, Receiver<T, N>)> {
    let inner = alloc::sync::Arc::new(ChannelInner {
        queue: Queue::<T, N>::new()?,
        tx_count: AtomicUsize::new(1),
        rx_count: AtomicUsize::new(1),
    });
    Ok((
        Sender {
            state: SenderState::Heap(inner.clone()),
        },
        Receiver {
            state: ReceiverState::Heap(inner),
        },
    ))
}

// ── Helpers shared between heap + static state ──────────────────────

#[inline]
fn queue_of<T: Copy + 'static, const N: usize>(s: &SenderState<T, N>) -> &Queue<T, N> {
    match s {
        #[cfg(all(feature = "alloc", not(zero_heap)))]
        SenderState::Heap(arc) => &arc.queue,
        SenderState::Static { queue, .. } => queue,
    }
}

#[inline]
fn rx_count_of<T: Copy + 'static, const N: usize>(s: &SenderState<T, N>) -> &AtomicUsize {
    match s {
        #[cfg(all(feature = "alloc", not(zero_heap)))]
        SenderState::Heap(arc) => &arc.rx_count,
        SenderState::Static { rx_count, .. } => rx_count,
    }
}

#[inline]
fn tx_count_of<T: Copy + 'static, const N: usize>(s: &SenderState<T, N>) -> &AtomicUsize {
    match s {
        #[cfg(all(feature = "alloc", not(zero_heap)))]
        SenderState::Heap(arc) => &arc.tx_count,
        SenderState::Static { tx_count, .. } => tx_count,
    }
}

#[inline]
fn queue_of_rx<T: Copy + 'static, const N: usize>(s: &ReceiverState<T, N>) -> &Queue<T, N> {
    match s {
        #[cfg(all(feature = "alloc", not(zero_heap)))]
        ReceiverState::Heap(arc) => &arc.queue,
        ReceiverState::Static { queue, .. } => queue,
    }
}

#[inline]
fn rx_count_of_rx<T: Copy + 'static, const N: usize>(s: &ReceiverState<T, N>) -> &AtomicUsize {
    match s {
        #[cfg(all(feature = "alloc", not(zero_heap)))]
        ReceiverState::Heap(arc) => &arc.rx_count,
        ReceiverState::Static { rx_count, .. } => rx_count,
    }
}

#[inline]
fn tx_count_of_rx<T: Copy + 'static, const N: usize>(s: &ReceiverState<T, N>) -> &AtomicUsize {
    match s {
        #[cfg(all(feature = "alloc", not(zero_heap)))]
        ReceiverState::Heap(arc) => &arc.tx_count,
        ReceiverState::Static { tx_count, .. } => tx_count,
    }
}

// ── Sender ──────────────────────────────────────────────────────────

impl<T: Copy + 'static, const N: usize> Sender<T, N> {
    /// Construct a sender from caller-owned static state. Use for
    /// zero-heap builds. The caller is responsible for ensuring the
    /// matching [`Receiver::from_static`] uses the same `queue`,
    /// `tx_count`, and `rx_count` and that both counts start at the
    /// correct initial value (typically 1 each for one sender + one
    /// receiver).
    ///
    /// # Safety
    /// Must be called with a queue and counts that are not already in
    /// use by another channel half.
    pub const unsafe fn from_static(
        queue: &'static Queue<T, N>,
        tx_count: &'static AtomicUsize,
        rx_count: &'static AtomicUsize,
    ) -> Self {
        Self {
            state: SenderState::Static {
                queue,
                tx_count,
                rx_count,
            },
        }
    }

    /// Send an item. Blocks until the queue accepts it, or returns
    /// [`Error::NetClosed`] if every [`Receiver`] has been dropped.
    pub fn send(&self, item: T) -> Result<()> {
        if rx_count_of(&self.state).load(Ordering::Acquire) == 0 {
            return Err(Error::NetClosed);
        }
        queue_of(&self.state).send(&item)
    }

    /// Non-blocking send. Returns [`Error::QueueFull`] when full,
    /// [`Error::NetClosed`] when all receivers are gone.
    pub fn try_send(&self, item: T) -> Result<()> {
        if rx_count_of(&self.state).load(Ordering::Acquire) == 0 {
            return Err(Error::NetClosed);
        }
        queue_of(&self.state).try_send(&item)
    }

    /// Number of currently-live [`Sender`] handles. Snapshot only —
    /// the count may change under concurrent clone/drop.
    pub fn sender_count(&self) -> usize {
        tx_count_of(&self.state).load(Ordering::Acquire)
    }

    /// Number of currently-live [`Receiver`] handles.
    pub fn receiver_count(&self) -> usize {
        rx_count_of(&self.state).load(Ordering::Acquire)
    }
}

impl<T: Copy + 'static, const N: usize> Clone for Sender<T, N> {
    fn clone(&self) -> Self {
        tx_count_of(&self.state).fetch_add(1, Ordering::AcqRel);
        Self {
            state: match &self.state {
                #[cfg(all(feature = "alloc", not(zero_heap)))]
                SenderState::Heap(arc) => SenderState::Heap(arc.clone()),
                SenderState::Static {
                    queue,
                    tx_count,
                    rx_count,
                } => SenderState::Static {
                    queue,
                    tx_count,
                    rx_count,
                },
            },
        }
    }
}

impl<T: Copy + 'static, const N: usize> Drop for Sender<T, N> {
    fn drop(&mut self) {
        tx_count_of(&self.state).fetch_sub(1, Ordering::AcqRel);
    }
}

// ── Receiver ────────────────────────────────────────────────────────

impl<T: Copy + 'static, const N: usize> Receiver<T, N> {
    /// Construct a receiver from caller-owned static state. See
    /// [`Sender::from_static`] for the safety + count-initialisation
    /// contract.
    ///
    /// # Safety
    /// Same as [`Sender::from_static`].
    pub const unsafe fn from_static(
        queue: &'static Queue<T, N>,
        tx_count: &'static AtomicUsize,
        rx_count: &'static AtomicUsize,
    ) -> Self {
        Self {
            state: ReceiverState::Static {
                queue,
                tx_count,
                rx_count,
            },
        }
    }

    /// Receive an item. Blocks until a sender posts, or returns
    /// [`Error::NetClosed`] when every [`Sender`] has been dropped *and*
    /// the queue is empty.
    pub fn recv(&self) -> Result<T> {
        // Fast path: try non-blocking first; if it returns an item we
        // don't care whether senders are still alive.
        if let Ok(v) = queue_of_rx(&self.state).try_recv() {
            return Ok(v);
        }
        if tx_count_of_rx(&self.state).load(Ordering::Acquire) == 0 {
            return Err(Error::NetClosed);
        }
        queue_of_rx(&self.state).recv()
    }

    /// Non-blocking receive. Returns the item if one is ready, otherwise
    /// [`TryRecvError::Empty`] when senders are still alive, or
    /// [`TryRecvError::Disconnected`] when the channel is empty *and*
    /// every [`Sender`] has been dropped.  Mirrors
    /// [`std::sync::mpsc::Receiver::try_recv`].
    pub fn try_recv(&self) -> core::result::Result<T, TryRecvError> {
        // Fast path: a successful non-blocking read wins regardless of
        // sender liveness (mirrors `recv`).
        if let Ok(v) = queue_of_rx(&self.state).try_recv() {
            return Ok(v);
        }
        // Empty now (QueueEmpty / Timeout / WouldBlock all mean "nothing
        // ready"): disambiguate momentarily-empty from disconnected by the
        // live-sender count.
        if tx_count_of_rx(&self.state).load(Ordering::Acquire) == 0 {
            Err(TryRecvError::Disconnected)
        } else {
            Err(TryRecvError::Empty)
        }
    }

    /// Number of currently-live [`Sender`] handles.
    pub fn sender_count(&self) -> usize {
        tx_count_of_rx(&self.state).load(Ordering::Acquire)
    }

    /// Number of currently-live [`Receiver`] handles.
    pub fn receiver_count(&self) -> usize {
        rx_count_of_rx(&self.state).load(Ordering::Acquire)
    }
}

impl<T: Copy + 'static, const N: usize> Clone for Receiver<T, N> {
    fn clone(&self) -> Self {
        rx_count_of_rx(&self.state).fetch_add(1, Ordering::AcqRel);
        Self {
            state: match &self.state {
                #[cfg(all(feature = "alloc", not(zero_heap)))]
                ReceiverState::Heap(arc) => ReceiverState::Heap(arc.clone()),
                ReceiverState::Static {
                    queue,
                    tx_count,
                    rx_count,
                } => ReceiverState::Static {
                    queue,
                    tx_count,
                    rx_count,
                },
            },
        }
    }
}

impl<T: Copy + 'static, const N: usize> Drop for Receiver<T, N> {
    fn drop(&mut self) {
        rx_count_of_rx(&self.state).fetch_sub(1, Ordering::AcqRel);
    }
}
