// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::time::Instant;
use ove::{AsyncEventGroup, Error, EventGroup, EventGroupStorage, WaitFlags};
use std::future::Future;
use std::sync::OnceLock;
use std::task::{Context, Poll, Waker};
use std::time::Duration;

static STORAGE: EventGroupStorage = EventGroupStorage::new();
static ASYNC: OnceLock<AsyncEventGroup> = OnceLock::new();
unsafe extern "C" {
    fn eventgroup_test_clocks() -> u32;
}

fn main() {
    let group = EventGroup::create(&STORAGE).unwrap();
    assert_eq!(EventGroup::valid_mask(), u32::MAX);
    assert_eq!(group.set_bits(u32::MAX), u32::MAX);
    assert_eq!(group.clear_bits(1), u32::MAX);
    assert_eq!(group.get_bits(), u32::MAX - 1);
    assert_eq!(group.clear_bits(EventGroup::valid_mask()), u32::MAX - 1);
    assert_eq!(group.set_bits(0), 0);
    assert_eq!(group.clear_bits(0), 0);
    assert_eq!(
        group.try_wait_bits(0, WaitFlags::NONE),
        Err(Error::InvalidParam)
    );
    assert_eq!(
        group.wait_bits_for(0, WaitFlags::NONE, Duration::ZERO),
        Err(Error::InvalidParam)
    );
    // Capture a finite deadline, then ensure invalid admission does not read again.
    let deadline = Instant::now();
    let before = unsafe { eventgroup_test_clocks() };
    assert_eq!(
        group.wait_bits_until(0, WaitFlags::NONE, deadline),
        Err(Error::InvalidParam)
    );
    assert_eq!(unsafe { eventgroup_test_clocks() }, before);
    assert_eq!(group.try_wait_bits(1, WaitFlags::NONE), Err(Error::Timeout));
    let asynchronous = ASYNC.get_or_init(|| AsyncEventGroup::new(group));
    // No callback is installed: this only exercises immediate invalid input.
    let mut future = std::pin::pin!(asynchronous.wait_bits(0, WaitFlags::NONE));
    let mut cx = Context::from_waker(Waker::noop());
    assert_eq!(
        future.as_mut().poll(&mut cx),
        Poll::Ready(Err(Error::InvalidParam))
    );
    println!("eventgroup rust: checks=13 failures=0");
}
