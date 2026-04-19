// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use core::fmt::Write;
use core::panic::PanicInfo;

use crate::fmt::FmtBuf;

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    let mut buf = [0u8; 256];
    let mut w = FmtBuf::new(&mut buf);
    let _ = write!(w, "[PANIC] {}\n", info);
    crate::log(w.as_bytes());
    loop {
        core::hint::spin_loop();
    }
}
