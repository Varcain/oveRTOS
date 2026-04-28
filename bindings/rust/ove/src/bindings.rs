// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
// bindgen-generated: literals come straight from C headers, so
// pedantic format/style lints don't apply to this module.
#![allow(clippy::unreadable_literal)]

include!(concat!(env!("OUT_DIR"), "/ove_bindings.rs"));
