// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Channel (Sender/Receiver) suite.
//!
//! Exercises the MPMC channel layered over `ove::Queue`, focused on the
//! non-blocking `Receiver::try_recv` shape and its `TryRecvError`
//! distinction between a momentarily-empty channel (`Empty`) and one
//! whose senders have all been dropped (`Disconnected`).

use crate::framework::run_suite;
use crate::test_entry;
use ove::channel::{self, TryRecvError};

fn test_try_recv_empty_then_item() {
    let (tx, rx) = channel::channel::<i32, 4>().unwrap();
    // Empty while a sender is still alive.
    assert_eq!(rx.try_recv(), Err(TryRecvError::Empty));
    tx.try_send(7).unwrap();
    assert_eq!(rx.try_recv(), Ok(7));
    // Drained again -> Empty (the sender has not been dropped).
    assert_eq!(rx.try_recv(), Err(TryRecvError::Empty));
}

fn test_try_recv_disconnected_drains_first() {
    let (tx, rx) = channel::channel::<i32, 4>().unwrap();
    tx.try_send(1).unwrap();
    drop(tx);
    // A buffered item still drains after the last sender is gone.
    assert_eq!(rx.try_recv(), Ok(1));
    // Now empty *and* all senders dropped -> Disconnected.
    assert_eq!(rx.try_recv(), Err(TryRecvError::Disconnected));
}

fn test_try_recv_multi_sender_disconnect() {
    let (tx, rx) = channel::channel::<i32, 4>().unwrap();
    let tx2 = tx.clone();
    drop(tx);
    // One sender still alive -> empty reads are `Empty`, not `Disconnected`.
    assert_eq!(rx.try_recv(), Err(TryRecvError::Empty));
    drop(tx2);
    assert_eq!(rx.try_recv(), Err(TryRecvError::Disconnected));
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Channel",
        &[
            test_entry!(test_try_recv_empty_then_item),
            test_entry!(test_try_recv_disconnected_drains_first),
            test_entry!(test_try_recv_multi_sender_disconnect),
        ],
    )
}
