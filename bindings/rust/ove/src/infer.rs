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

    /// Create from caller-provided storage and arena.
    ///
    /// Available in both heap and zero-heap modes.  Useful when the same
    /// storage/arena must be reused for different models (e.g. two-stage
    /// inference pipelines).
    ///
    /// # Safety
    /// Caller must ensure `storage` and `arena` outlive the `Model` and are
    /// not shared with another primitive.
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

// ---------------------------------------------------------------------------
// ModelStorage — safe reusable arena
// ---------------------------------------------------------------------------

/// Reusable model storage and arena pair for sequential inference.
///
/// Owns both the C `ove_model_storage_t` and a 16-byte aligned arena
/// buffer.  Call [`load()`](ModelStorage::load) to create a [`Model`]
/// session; the borrow checker ensures the storage is not shared.
///
/// # Example
///
/// ```ignore
/// let mut storage = ModelStorage::<32768>::new();
/// let model = storage.load(&cfg)?;
/// let input = model.input_slice_mut::<i16>(0)?;
/// input[0] = 42;
/// model.invoke()?;
/// let output = model.output_slice::<i8>(0)?;
/// ```
#[repr(C, align(16))]
pub struct ModelStorage<const ARENA_SIZE: usize> {
    storage: bindings::ove_model_storage_t,
    arena: [u8; ARENA_SIZE],
}

impl<const ARENA_SIZE: usize> ModelStorage<ARENA_SIZE> {
    /// Create a zeroed storage + arena pair.
    pub fn new() -> Self {
        // SAFETY: ove_model_storage_t is a C struct that is valid when zeroed.
        unsafe {
            core::mem::zeroed()
        }
    }

    /// Load a model into this storage, returning a session handle.
    ///
    /// The arena size is supplied by the const generic `ARENA_SIZE` —
    /// no need to repeat it.  The returned [`Model`] borrows `self`
    /// mutably, so the compiler prevents concurrent use or re-loading
    /// until the model is dropped.
    pub fn load(&mut self, model_data: &[u8]) -> Result<Model> {
        let config = ModelConfig {
            model_data,
            arena_size: ARENA_SIZE,
        };
        unsafe {
            Model::from_static(
                &mut self.storage,
                self.arena.as_mut_ptr(),
                &config,
            )
        }
    }
}

// ---------------------------------------------------------------------------
// Typed tensor accessors
// ---------------------------------------------------------------------------

impl Model {
    /// Get input tensor data as a mutable typed slice.
    ///
    /// The slice length is `tensor_info.size / size_of::<T>()`.
    ///
    /// # Errors
    /// Returns an error if the tensor index is invalid.
    pub fn input_slice_mut<T>(&self, index: u32) -> Result<&mut [T]> {
        let info = self.input(index)?;
        let count = info.size / core::mem::size_of::<T>();
        // SAFETY: The tensor arena is owned by the model session and
        // valid for the lifetime of this Model.  We have &self so the
        // model is alive.  The caller must not alias this slice with
        // another call to input_slice_mut for the same tensor index.
        Ok(unsafe {
            core::slice::from_raw_parts_mut(info.data as *mut T, count)
        })
    }

    /// Get output tensor data as a typed slice.
    ///
    /// The slice length is `tensor_info.size / size_of::<T>()`.
    ///
    /// # Errors
    /// Returns an error if the tensor index is invalid.
    pub fn output_slice<T>(&self, index: u32) -> Result<&[T]> {
        let info = self.output(index)?;
        let count = info.size / core::mem::size_of::<T>();
        // SAFETY: Same as input_slice_mut, but immutable.
        Ok(unsafe {
            core::slice::from_raw_parts(info.data as *const T, count)
        })
    }
}
