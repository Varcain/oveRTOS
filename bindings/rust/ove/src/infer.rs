// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! ML inference primitives for oveRTOS.
//!
//! Provides a safe Rust wrapper around the `ove_model_*` C API for
//! running TFLite model inference.

use crate::bindings;
use crate::error::{Error, Result};

/// Tensor element types matching the C `enum ove_tensor_type`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TensorType {
    Float32 = 0,
    Int8 = 1,
    Uint8 = 2,
    Int16 = 3,
    Int32 = 4,
}

/// Tensor metadata descriptor.
#[derive(Debug, Clone)]
pub struct TensorInfo {
    /// Pointer to tensor data in the arena.
    pub data: *mut u8,
    /// Total size in bytes.
    pub size: usize,
    /// Element type.
    pub tensor_type: TensorType,
    /// Number of dimensions.
    pub ndims: u32,
    /// Shape array (up to 5 dimensions).
    pub dims: [i32; 5],
}

/// Model configuration.
pub struct ModelConfig<'a> {
    /// Reference to the .tflite FlatBuffer data.
    pub model_data: &'a [u8],
    /// Tensor arena size in bytes.
    pub arena_size: usize,
}

/// An ML inference model session.
///
/// Wraps a TFLM `MicroInterpreter` with automatic cleanup.
pub struct Model {
    handle: bindings::ove_model_t,
}

impl Model {
    /// Create a new model via heap allocation.
    #[cfg(not(zero_heap))]
    pub fn new(config: &ModelConfig) -> Result<Self> {
        let mut handle: bindings::ove_model_t = core::ptr::null_mut();
        let c_cfg = bindings::ove_model_config {
            model_data: config.model_data.as_ptr() as *const core::ffi::c_void,
            model_size: config.model_data.len(),
            arena_size: config.arena_size,
        };
        let rc = unsafe { bindings::ove_model_create(&mut handle, &c_cfg) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage and arena.
    ///
    /// # Safety
    /// Caller must ensure `storage` and `arena` outlive the `Model` and are
    /// not shared with another primitive.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_model_storage_t,
        arena: *mut u8,
        config: &ModelConfig,
    ) -> Result<Self> {
        let mut handle: bindings::ove_model_t = core::ptr::null_mut();
        let c_cfg = bindings::ove_model_config {
            model_data: config.model_data.as_ptr() as *const core::ffi::c_void,
            model_size: config.model_data.len(),
            arena_size: config.arena_size,
        };
        let rc = bindings::ove_model_init(
            &mut handle,
            storage,
            arena as *mut core::ffi::c_void,
            &c_cfg,
        );
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Run the model forward pass.
    pub fn invoke(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_model_invoke(self.handle) };
        Error::from_code(rc)
    }

    /// Get tensor info for an input tensor.
    pub fn input(&self, index: u32) -> Result<TensorInfo> {
        let mut info: bindings::ove_tensor_info = unsafe { core::mem::zeroed() };
        let rc = unsafe {
            bindings::ove_model_input(self.handle, index, &mut info)
        };
        Error::from_code(rc)?;
        Ok(TensorInfo {
            data: info.data as *mut u8,
            size: info.size,
            tensor_type: match info.type_ {
                1 => TensorType::Int8,
                2 => TensorType::Uint8,
                3 => TensorType::Int16,
                4 => TensorType::Int32,
                _ => TensorType::Float32,
            },
            ndims: info.ndims,
            dims: info.dims,
        })
    }

    /// Get tensor info for an output tensor.
    pub fn output(&self, index: u32) -> Result<TensorInfo> {
        let mut info: bindings::ove_tensor_info = unsafe { core::mem::zeroed() };
        let rc = unsafe {
            bindings::ove_model_output(self.handle, index, &mut info)
        };
        Error::from_code(rc)?;
        Ok(TensorInfo {
            data: info.data as *mut u8,
            size: info.size,
            tensor_type: match info.type_ {
                1 => TensorType::Int8,
                2 => TensorType::Uint8,
                3 => TensorType::Int16,
                4 => TensorType::Int32,
                _ => TensorType::Float32,
            },
            ndims: info.ndims,
            dims: info.dims,
        })
    }

    /// Return last inference duration in microseconds.
    pub fn last_inference_us(&self) -> u64 {
        unsafe { bindings::ove_model_last_inference_us(self.handle) }
    }
}

impl Drop for Model {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            #[cfg(not(zero_heap))]
            unsafe {
                bindings::ove_model_destroy(self.handle);
            }
            #[cfg(zero_heap)]
            unsafe {
                bindings::ove_model_deinit(self.handle);
            }
        }
    }
}

unsafe impl Send for Model {}
unsafe impl Sync for Model {}
