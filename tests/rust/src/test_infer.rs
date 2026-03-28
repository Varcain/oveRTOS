// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! ML inference module tests.
//!
//! When CONFIG_OVE_INFER is not defined, the C stubs are static inline
//! and not available via FFI.  These tests verify the Rust binding types
//! compile correctly and the API shape matches expectations.

use crate::framework::run_suite;
use crate::test_entry;

fn test_infer_types_exist() {
    // Verify the binding types are accessible
    let _: ove::ffi::ove_model_t = core::ptr::null_mut();

    // Verify ove_model_config struct layout
    let cfg: ove::ffi::ove_model_config = unsafe { core::mem::zeroed() };
    assert_eq!(cfg.model_size, 0);
    assert_eq!(cfg.arena_size, 0);
}

fn test_infer_tensor_info_layout() {
    // Verify ove_tensor_info struct is accessible and zeroed correctly
    let info: ove::ffi::ove_tensor_info = unsafe { core::mem::zeroed() };
    assert_eq!(info.size, 0);
    assert_eq!(info.ndims, 0);
}

pub fn run() -> (usize, usize) {
    run_suite("Inference", &[
        test_entry!(test_infer_types_exist),
        test_entry!(test_infer_tensor_info_layout),
    ])
}
