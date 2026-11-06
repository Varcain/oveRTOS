// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Borrowed model assets, owned sessions and checked tensor views.
//!
//! [`ModelConfig::from_trusted_model`] qualifies the native program and exact
//! arena configuration. [`Model::invoke`] separately requires a qualified
//! numeric input/state domain. Native TFLM is unmodified; alignment/schema
//! checks do not validate arbitrary programs or intermediate arena capacities.
//! Measure setup on the target/provider, keep at least `arena_used() + 16`,
//! retain the qualified preparation capacity, and requalify after changes.
//!
//! Sessions retain immutable model bytes and exclusive borrowed storage until
//! close. Writers require exclusive access; shared metadata/usage queries are
//! read-only. Native setup clears the effective arena before constructing SDK
//! objects. Typed views validate scalar type and retained backing before forming
//! references; outputs can also refer to immutable model-backed constants.
//!
//! Construction and invocation reject detected ISR context with InvalidParam.
//! Drop normally closes synchronously. In ISR context it retains native resources
//! and admission indefinitely, as does `mem::forget`; later construction returns
//! Busy without accessing abandoned backing. There is no background worker or
//! recovery. Host POSIX uses ordinary threads and reports no hardware ISR.

use core::{ffi::c_void, marker::PhantomData, mem, ptr};

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

mod sealed {
    pub trait Sealed {}
    impl Sealed for f32 {}
    impl Sealed for i8 {}
    impl Sealed for u8 {}
    impl Sealed for i16 {}
    impl Sealed for i32 {}
}

/// Supported all-bit-pattern-valid tensor scalars. Implementations are sealed.
pub trait TensorElement: sealed::Sealed + Copy {
    /// Exact native tensor type required by a view.
    const TYPE: TensorType;
}
macro_rules! scalar {
    ($ty:ty, $kind:ident) => {
        impl TensorElement for $ty {
            const TYPE: TensorType = TensorType::$kind;
        }
    };
}
scalar!(f32, Float32);
scalar!(i8, Int8);
scalar!(u8, Uint8);
scalar!(i16, Int16);
scalar!(i32, Int32);

/// Raw tensor metadata. Dereferencing `data` requires unsafe integration.
///
/// Supported descriptors report exact rank zero through five and zero unused
/// dimensions. Outputs are read-only, including model-backed constants. A raw
/// pointer value does not extend the session's backing lifetime.
#[derive(Debug, Clone)]
pub struct TensorInfo {
    /// Tensor data pointer; output data must not be written.
    pub data: *mut u8,
    /// Total size in bytes.
    pub size: usize,
    /// Exact element type.
    pub tensor_type: TensorType,
    /// Rank, from zero through five.
    pub ndims: u32,
    /// Exact shape with unused entries zero.
    pub dims: [i32; 5],
}

/// Qualified native program and exact configured arena capacity.
///
/// Private fields prevent safe code from replacing the program or shrinking a
/// qualified arena. Creating sessions borrows the bytes, not this config object.
#[derive(Debug, Clone, Copy)]
pub struct ModelConfig<'model> {
    model_data: &'model [u8],
    arena_size: usize,
}
impl<'model> ModelConfig<'model> {
    /// Qualify a trusted build-time model for this native build and arena size.
    ///
    /// Rejects empty/misaligned assets and zero or unrepresentable capacities.
    /// These cheap checks do not establish the safety requirements below.
    ///
    /// # Safety
    /// The bytes must describe a valid supported TFLite program whose setup and
    /// teardown have defined behavior in this target/provider/TFLM build. The
    /// exact capacity must meet persistent and peak preparation requirements,
    /// including any front padding the native allocator can require (up to 15
    /// bytes). Intermediate undersized arenas can cause undefined behavior.
    /// Setup must leave valid tensor metadata and initialized tensor backing.
    /// Safe tensor views independently reject spans outside the retained
    /// arena or immutable model bytes.
    /// Registered callbacks must obey the synchronous, externally serialized
    /// session contract and retain no autonomous accesses after abandonment.
    /// Numeric domains for each invocation are a separate obligation of invoke.
    pub unsafe fn from_trusted_model(model_data: &'model [u8], arena_size: usize) -> Result<Self> {
        if model_data.is_empty()
            || model_data.as_ptr() as usize % 16 != 0
            || arena_size == 0
            || arena_size > isize::MAX as usize
        {
            return Err(Error::InvalidParam);
        }
        Ok(Self {
            model_data,
            arena_size,
        })
    }

    /// Qualified configured arena capacity, before native front padding.
    pub fn arena_size(&self) -> usize {
        self.arena_size
    }

    /// Immutable borrowed model asset.
    pub fn model_data(&self) -> &'model [u8] {
        self.model_data
    }

    fn native(&self) -> bindings::ove_model_config {
        bindings::ove_model_config {
            model_data: self.model_data.as_ptr().cast(),
            model_size: self.model_data.len(),
            arena_size: self.arena_size,
        }
    }
}

#[derive(Debug, Clone, Copy)]
struct Arena {
    base: *mut u8,
    size: usize,
}

// Check integer addresses only to select an offset, then derive the reference
// pointer from the retained allocation base, preserving its provenance.
fn offset_in(base: *const u8, capacity: usize, data: *const u8, size: usize) -> Option<usize> {
    let offset = (data as usize).checked_sub(base as usize)?;
    if offset <= capacity && size <= capacity - offset {
        Some(offset)
    } else {
        None
    }
}

/// An exclusively owned synchronous session retaining model and storage loans.
///
/// Moving the owner leaves its native backing in place. Forgetting it, or
/// dropping it in ISR context, permanently retains admission and native heap
/// allocations. Borrowed backing may then leave scope: no native worker runs,
/// and later constructors inspect only the admission boolean before returning
/// Busy. Normal Drop releases the session exactly once.
#[derive(Debug)]
pub struct Model<'storage, 'model> {
    handle: bindings::ove_model_t,
    arena: Arena,
    model_data: &'model [u8],
    heap_owned: bool,
    storage: PhantomData<&'storage mut ()>,
}

// SAFETY: The session is synchronous, without task affinity or autonomous
// workers. Moving the owner keeps backing in place. Every writer requires
// &mut self; all shared native paths only read eagerly initialized metadata.
// Native global admission serializes setup/teardown, including failure tails.
unsafe impl Send for Model<'_, '_> {}
unsafe impl Sync for Model<'_, '_> {}

impl<'model> Model<'static, 'model> {
    /// Allocate a session using the native allocator, retaining the asset loan.
    ///
    /// Returns InvalidParam in detected ISR context, and native errors otherwise.
    #[cfg(not(zero_heap))]
    pub fn new(config: &ModelConfig<'model>) -> Result<Self> {
        check_thread()?;
        let mut handle = ptr::null_mut();
        // SAFETY: The token qualifies this setup; native owns the allocations.
        // finish retains the asset, and normal Drop closes once before release.
        Error::from_code(unsafe { bindings::ove_model_create(&mut handle, &config.native()) })?;
        // SAFETY: Native success returned one live owned handle.
        unsafe { Self::finish(handle, true, config) }
    }
}

fn check_thread() -> Result<()> {
    // SAFETY: This pointer-free query is available independently of ASYNC and
    // accesses no handle or retained backing, including during ISR Drop.
    if unsafe { bindings::ove_model_binding_in_isr() } {
        Err(Error::InvalidParam)
    } else {
        Ok(())
    }
}

impl<'storage, 'model> Model<'storage, 'model> {
    /// Integrate caller-provided native storage and arena.
    ///
    /// Safe applications normally use ModelStorage::load. This function checks
    /// context and retains the model asset loan, but raw backing is unchecked.
    ///
    /// # Safety
    /// Storage must be correctly aligned, initialized storage for one native
    /// model record and remain exclusively borrowed for 'storage. Arena must
    /// be one writable initialized allocation of at least config.arena_size()
    /// bytes, valid and exclusively borrowed for 'storage; it must not overlap
    /// storage or immutable model bytes. Both must remain in place until normal
    /// close completes, or until this owner is forgotten/dropped in ISR. No
    /// external alias may access the handle or backing while the owner exists.
    /// The qualified program/configuration contract still applies. Do not
    /// recover or call native operations through abandoned raw handles.
    pub unsafe fn from_static(
        storage: *mut bindings::ove_model_storage_t,
        arena: *mut u8,
        config: &ModelConfig<'model>,
    ) -> Result<Self> {
        check_thread()?;
        let mut handle = ptr::null_mut();
        // SAFETY: Caller supplies the exclusive truthful backing and config
        // qualifies setup. Native success transfers its session to this owner.
        Error::from_code(unsafe {
            bindings::ove_model_init(&mut handle, storage, arena.cast(), &config.native())
        })?;
        // SAFETY: One live borrowed handle, with backing retained by the caller.
        unsafe { Self::finish(handle, false, config) }
    }

    unsafe fn finish(
        handle: bindings::ove_model_t,
        heap_owned: bool,
        config: &ModelConfig<'model>,
    ) -> Result<Self> {
        let mut model = Self {
            handle,
            arena: Arena {
                base: ptr::null_mut(),
                size: 0,
            },
            model_data: config.model_data,
            heap_owned,
            storage: PhantomData,
        };
        let mut base: *mut c_void = ptr::null_mut();
        let mut size = 0;
        // SAFETY: Constructor success produced this exclusive live handle. The
        // helper only reads its exact effective arena span, excluding padding.
        Error::from_code(unsafe {
            bindings::ove_model_binding_arena(handle, &mut base, &mut size)
        })?;
        if base.is_null()
            || size > isize::MAX as usize
            || (base as usize).checked_add(size).is_none()
        {
            return Err(Error::InvalidParam);
        }
        model.arena = Arena {
            base: base.cast(),
            size,
        };
        Ok(model)
    }

    /// Run a qualified forward pass with exclusive session access.
    ///
    /// # Safety
    /// All input values and mutable model state must lie in a domain for which
    /// this exact program/target/provider execution has defined behavior. This
    /// includes intermediate arithmetic, allocation and kernel assumptions.
    /// Success or an ordinary native error must leave valid native metadata and
    /// initialized tensor backing for later queries and close. An error return
    /// does not make an unqualified call safe. Returns InvalidParam in ISR.
    pub unsafe fn invoke(&mut self) -> Result<()> {
        check_thread()?;
        // SAFETY: Caller qualifies numeric execution; &mut self excludes live
        // views/concurrent access. Program and backing are retained by self.
        Error::from_code(unsafe { bindings::ove_model_invoke(self.handle) })
    }

    fn tensor(&self, index: u32, output: bool) -> Result<TensorInfo> {
        // SAFETY: The C descriptor is valid when zero initialized.
        let mut raw: bindings::ove_tensor_info = unsafe { mem::zeroed() };
        // SAFETY: Live retained handle, writable local descriptor, read-only
        // eager metadata lookup. Shared queries perform no native mutation.
        let rc = unsafe {
            if output {
                bindings::ove_model_output(self.handle, index, &mut raw)
            } else {
                bindings::ove_model_input(self.handle, index, &mut raw)
            }
        };
        Error::from_code(rc)?;
        let tensor_type = match raw.type_ {
            0 => TensorType::Float32,
            1 => TensorType::Int8,
            2 => TensorType::Uint8,
            3 => TensorType::Int16,
            4 => TensorType::Int32,
            _ => return Err(Error::NotSupported),
        };
        Ok(TensorInfo {
            data: raw.data.cast(),
            size: raw.size,
            tensor_type,
            ndims: raw.ndims,
            dims: raw.dims,
        })
    }

    /// Read exact raw input metadata. Unsupported types/ranks return NotSupported.
    pub fn input(&self, index: u32) -> Result<TensorInfo> {
        self.tensor(index, false)
    }

    /// Read exact raw, read-only output metadata, including constant outputs.
    pub fn output(&self, index: u32) -> Result<TensorInfo> {
        self.tensor(index, true)
    }

    /// Last inference duration, or zero before the first invocation.
    pub fn last_inference_us(&self) -> u64 {
        // SAFETY: Live handle and shared read-only native query.
        unsafe { bindings::ove_model_last_inference_us(self.handle) }
    }

    /// Native usage including original alignment padding. Keep usage plus 16
    /// and the qualified peak preparation capacity; requalify after changes.
    pub fn arena_used(&self) -> usize {
        // SAFETY: Live handle and shared read-only native query.
        unsafe { bindings::ove_model_arena_used(self.handle) }
    }

    fn validate<T: TensorElement>(info: &TensorInfo) -> Result<usize> {
        if info.tensor_type != T::TYPE
            || info.size > isize::MAX as usize
            || info.size % mem::size_of::<T>() != 0
        {
            return Err(Error::InvalidParam);
        }
        if info.size != 0 && (info.data.is_null() || info.data as usize % mem::align_of::<T>() != 0)
        {
            return Err(Error::InvalidParam);
        }
        Ok(info.size / mem::size_of::<T>())
    }

    /// Borrow an exclusive input view of the exact supported scalar type.
    ///
    /// Returns InvalidParam for mismatched type, alignment, incomplete elements,
    /// unrepresentable lengths or backing outside the effective writable arena.
    /// The borrow excludes any other view, invocation, reload or close.
    pub fn input_slice_mut<T: TensorElement>(&mut self, index: u32) -> Result<&mut [T]> {
        let info = self.input(index)?;
        let count = Self::validate::<T>(&info)?;
        if count == 0 {
            return Ok(&mut []);
        }
        let offset = offset_in(self.arena.base, self.arena.size, info.data, info.size)
            .ok_or(Error::InvalidParam)?;
        // SAFETY: Exact type is all-bit-pattern-valid, span/alignment and isize
        // length are checked within one exclusive retained initialized arena.
        // Native setup clears that entire span; qualified invocations retain
        // initialization. &mut self excludes all competing tensor references.
        Ok(unsafe { core::slice::from_raw_parts_mut(self.arena.base.add(offset).cast(), count) })
    }

    /// Borrow a shared output view of the exact supported scalar type.
    ///
    /// Checks the same representation requirements as input views. Backing must
    /// be in the effective arena or immutable retained model bytes. The borrow
    /// excludes input mutation, invocation, reload and close; stale initialized
    /// output bytes may be read before an invocation or after an ordinary error.
    pub fn output_slice<T: TensorElement>(&self, index: u32) -> Result<&[T]> {
        let info = self.output(index)?;
        let count = Self::validate::<T>(&info)?;
        if count == 0 {
            return Ok(&[]);
        }
        let data = if let Some(offset) =
            offset_in(self.arena.base, self.arena.size, info.data, info.size)
        {
            // SAFETY: Offset and full nonempty span are within retained arena.
            unsafe { self.arena.base.add(offset).cast_const() }
        } else {
            let offset = offset_in(
                self.model_data.as_ptr(),
                self.model_data.len(),
                info.data,
                info.size,
            )
            .ok_or(Error::InvalidParam)?;
            // SAFETY: Offset and full nonempty span are within retained bytes.
            unsafe { self.model_data.as_ptr().add(offset) }
        };
        // SAFETY: Exact all-bit-pattern-valid scalar, alignment/isize and one
        // initialized retained allocation checked. &self excludes all writers.
        Ok(unsafe { core::slice::from_raw_parts(data.cast(), count) })
    }
}

impl Drop for Model<'_, '_> {
    fn drop(&mut self) {
        if check_thread().is_err() {
            return;
        }
        // SAFETY: Sole live owner. No views survive this exclusive Drop. Native
        // closes synchronously before either owned allocation or loan is freed.
        // The ISR branch above does not access native handles or free backing.
        unsafe {
            if self.heap_owned {
                #[cfg(not(zero_heap))]
                bindings::ove_model_destroy(self.handle);
            } else {
                bindings::ove_model_deinit(self.handle);
            }
        }
    }
}

#[repr(align(16))]
struct AlignedArena<const N: usize>([u8; N]);

/// Reusable native storage and an individually 16-byte aligned arena.
///
/// A live Model exclusively borrows this pair and independently borrows its
/// asset. Drop the session to reuse storage with another qualified config.
#[repr(C, align(16))]
pub struct ModelStorage<const ARENA_SIZE: usize> {
    storage: bindings::ove_model_storage_t,
    arena: AlignedArena<ARENA_SIZE>,
}
impl<const N: usize> Default for ModelStorage<N> {
    fn default() -> Self {
        Self::new()
    }
}
impl<const N: usize> ModelStorage<N> {
    /// Const zero initialization, suitable for static placement.
    pub const fn new() -> Self {
        // SAFETY: Native opaque storage is byte backing, valid when zeroed.
        Self {
            storage: unsafe { mem::zeroed() },
            arena: AlignedArena([0; N]),
        }
    }

    /// Load the exact qualified configuration, retaining both backing loans.
    ///
    /// Returns InvalidParam if storage is smaller than the qualified arena or
    /// in detected ISR context, before native effects. A larger storage buffer
    /// still uses exactly the qualified configured capacity.
    pub fn load<'storage, 'model>(
        &'storage mut self,
        config: &ModelConfig<'model>,
    ) -> Result<Model<'storage, 'model>> {
        check_thread()?;
        if config.arena_size > N {
            return Err(Error::InvalidParam);
        }
        // SAFETY: Fields are disjoint initialized regions of this exclusive
        // storage loan, with an individually aligned arena; config fits N.
        unsafe { Model::from_static(&mut self.storage, self.arena.0.as_mut_ptr(), config) }
    }
}
