// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

pub unsafe fn callback_bytes<'a>(ptr: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        return Some(&[]);
    }
    if ptr.is_null() {
        return None;
    }

    // SAFETY: The caller guarantees that `ptr` is valid for `len` bytes.
    Some(unsafe { core::slice::from_raw_parts(ptr, len) })
}

pub fn topic_str_from_bytes(topic: &[u8]) -> Option<&str> {
    core::str::from_utf8(topic).ok()
}
