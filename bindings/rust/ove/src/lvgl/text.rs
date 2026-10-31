// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use super::{Label, Widget, bindings};
use crate::{Error, Result};
use core::cell::UnsafeCell;
use core::ffi::{c_char, c_void};
use core::fmt::{self, Write};
use core::ptr;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, AtomicUsize, Ordering};

// Short conversions stay on the stack. Larger ones use LVGL's fixed pool.
pub(super) struct TextBuffer {
    stack: [u8; 128],
    heap: *mut u8,
    capacity: usize,
    len: usize,
    error: Option<Error>,
}

impl TextBuffer {
    pub(super) fn new() -> Self {
        Self {
            stack: [0; 128],
            heap: ptr::null_mut(),
            capacity: 128,
            len: 0,
            error: None,
        }
    }

    pub(super) fn as_ptr(&self) -> *const c_char {
        if self.heap.is_null() {
            self.stack.as_ptr().cast()
        } else {
            self.heap.cast()
        }
    }

    pub(super) fn formatted(args: fmt::Arguments<'_>) -> Result<Self> {
        let mut buffer = Self::new();
        if fmt::write(&mut buffer, args).is_err() || buffer.error.is_some() {
            return Err(buffer.error.unwrap_or(Error::InvalidParam));
        }
        Ok(buffer)
    }
}

impl Write for TextBuffer {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        if self.error.is_some() {
            return Err(fmt::Error);
        }
        let result = (|| {
            if text.as_bytes().contains(&0) {
                return Err(Error::InvalidParam);
            }
            let needed = self
                .len
                .checked_add(text.len())
                .and_then(|n| n.checked_add(1))
                .ok_or(Error::NoMemory)?;
            if needed > self.capacity {
                let capacity = needed.max(self.capacity.saturating_mul(2));
                // SAFETY: the LVGL lock is held; heap is null or our allocation.
                let heap = unsafe { bindings::ove_lvgl_text_realloc(self.heap.cast(), capacity) }
                    .cast::<u8>();
                if heap.is_null() {
                    return Err(Error::NoMemory);
                }
                if self.heap.is_null() {
                    // SAFETY: both regions are valid and disjoint for len bytes.
                    unsafe { ptr::copy_nonoverlapping(self.stack.as_ptr(), heap, self.len) };
                }
                self.heap = heap;
                self.capacity = capacity;
            }
            let dest = if self.heap.is_null() {
                self.stack.as_mut_ptr()
            } else {
                self.heap
            };
            // SAFETY: capacity includes the terminator; source is a valid str.
            unsafe {
                ptr::copy_nonoverlapping(text.as_ptr(), dest.add(self.len), text.len());
                self.len += text.len();
                *dest.add(self.len) = 0;
            }
            Ok(())
        })();
        if let Err(error) = result {
            self.error = Some(error);
        }
        result.map_err(|_| fmt::Error)
    }
}

impl Drop for TextBuffer {
    fn drop(&mut self) {
        // SAFETY: null or the allocation owned exclusively by this buffer.
        unsafe { bindings::ove_lvgl_text_free(self.heap.cast()) };
    }
}

pub(super) fn with_text<T>(text: &str, apply: impl FnOnce(*const c_char) -> T) -> Result<T> {
    let mut buffer = TextBuffer::new();
    buffer
        .write_str(text)
        .map_err(|_| buffer.error.unwrap_or(Error::InvalidParam))?;
    Ok(apply(buffer.as_ptr()))
}

pub(super) struct Watch(*mut bindings::ove_lvgl_text_watch_t);
impl Watch {
    pub(super) fn new(label: impl Widget) -> Result<Self> {
        // SAFETY: synchronous lifetime watch, no user data to release.
        let raw =
            unsafe { bindings::ove_lvgl_text_watch(label.raw().cast(), ptr::null_mut(), None) };
        if raw.is_null() {
            Err(Error::NoMemory)
        } else {
            Ok(Self(raw))
        }
    }
    pub(super) fn check(&self) -> Result<()> {
        // SAFETY: the caller reference keeps the watch alive after deletion.
        if unsafe { bindings::ove_lvgl_text_watch_valid(self.0) } == 0 {
            Err(Error::BadHandle)
        } else {
            Ok(())
        }
    }
}
impl Drop for Watch {
    fn drop(&mut self) {
        // SAFETY: release the caller reference and any remaining event hook.
        unsafe { bindings::ove_lvgl_text_watch_end(self.0) };
    }
}

/// Stable label storage. `N` includes the trailing terminator.
///
/// Declare this as a static. Attachment allocates one small LVGL lifecycle
/// record; subsequent updates allocate no text memory. Two buffers keep the
/// displayed text unchanged if formatting fails or exceeds capacity.
/// Like native static labels, this mode bypasses Arabic/Persian preprocessing
/// and does not support dots long mode. Use copied text for those features.
pub struct TextStorage<const N: usize> {
    buffers: [UnsafeCell<[u8; N]>; 2],
    owner: AtomicPtr<bindings::lv_obj_t>,
    generation: AtomicU32,
    active: AtomicUsize,
    busy: AtomicBool,
}

// SAFETY: LVGL operations require its lock; busy additionally excludes
// reentrant writes, and Rust never exposes references into the buffers.
unsafe impl<const N: usize> Sync for TextStorage<N> {}

impl<const N: usize> Default for TextStorage<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> TextStorage<N> {
    pub const fn new() -> Self {
        Self {
            buffers: [UnsafeCell::new([0; N]), UnsafeCell::new([0; N])],
            owner: AtomicPtr::new(ptr::null_mut()),
            generation: AtomicU32::new(0),
            active: AtomicUsize::new(0),
            busy: AtomicBool::new(false),
        }
    }

    fn enter(&self) -> Result<BusyGuard<'_>> {
        self.busy
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| Error::Busy)?;
        Ok(BusyGuard(&self.busy))
    }

    pub(super) fn attach(
        &'static self,
        label: Label,
        args: fmt::Arguments<'_>,
    ) -> Result<BufferedLabel<N>> {
        let _guard = self.enter()?;
        if !self.owner.load(Ordering::Acquire).is_null() {
            return Err(Error::Busy);
        }
        let generation = self
            .generation
            .load(Ordering::Relaxed)
            .checked_add(1)
            .ok_or(Error::BadHandle)?;
        let watch = Watch::new(label)?;
        let index = 1 - self.active.load(Ordering::Relaxed);
        self.format(index, args)?;
        watch.check()?;
        self.generation.store(generation, Ordering::Release);
        self.owner.store(label.raw(), Ordering::Release);
        // SAFETY: static storage remains alive until deletion/replacement, when
        // release invalidates the token. busy protects the staging buffer.
        let result = unsafe {
            bindings::ove_lvgl_text_static(
                label.raw().cast(),
                self.buffers[index].get().cast(),
                (self as *const Self).cast_mut().cast(),
                Some(release::<N>),
            )
        };
        if let Err(error) = Error::from_code(result) {
            self.owner.store(ptr::null_mut(), Ordering::Release);
            return Err(error);
        }
        self.active.store(index, Ordering::Relaxed);
        Ok(BufferedLabel {
            label,
            storage: self,
            generation,
        })
    }

    fn format(&self, index: usize, args: fmt::Arguments<'_>) -> Result<()> {
        // SAFETY: caller holds busy; this is the inactive, unpublished buffer.
        let bytes = unsafe { &mut *self.buffers[index].get() };
        if bytes.is_empty() {
            return Err(Error::NoSpace);
        }
        let mut writer = FixedWriter {
            bytes,
            len: 0,
            error: None,
        };
        if fmt::write(&mut writer, args).is_err() || writer.error.is_some() {
            return Err(writer.error.unwrap_or(Error::InvalidParam));
        }
        writer.bytes[writer.len] = 0;
        Ok(())
    }
}

struct BusyGuard<'a>(&'a AtomicBool);
impl Drop for BusyGuard<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

struct FixedWriter<'a> {
    bytes: &'a mut [u8],
    len: usize,
    error: Option<Error>,
}
impl Write for FixedWriter<'_> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        if self.error.is_some() {
            return Err(fmt::Error);
        }
        let error = if text.as_bytes().contains(&0) {
            Some(Error::InvalidParam)
        } else if text.len() >= self.bytes.len() - self.len {
            Some(Error::NoSpace)
        } else {
            None
        };
        if let Some(error) = error {
            self.error = Some(error);
            return Err(fmt::Error);
        }
        self.bytes[self.len..self.len + text.len()].copy_from_slice(text.as_bytes());
        self.len += text.len();
        Ok(())
    }
}

unsafe extern "C" fn release<const N: usize>(data: *mut c_void) {
    // SAFETY: attached records point only to a static TextStorage<N>.
    let storage = unsafe { &*data.cast::<TextStorage<N>>() };
    storage.owner.store(ptr::null_mut(), Ordering::Release);
}

/// A label attached to binding-managed text storage. Replacement or deletion
/// invalidates this token. Updates require the LVGL lock, like other widgets.
#[derive(Clone, Copy)]
pub struct BufferedLabel<const N: usize> {
    label: Label,
    storage: &'static TextStorage<N>,
    generation: u32,
}

impl<const N: usize> BufferedLabel<N> {
    pub fn label(self) -> Label {
        self.label
    }

    fn check(&self) -> Result<()> {
        if self.storage.owner.load(Ordering::Acquire) != self.label.raw()
            || self.storage.generation.load(Ordering::Acquire) != self.generation
        {
            Err(Error::BadHandle)
        } else {
            Ok(())
        }
    }

    /// Format without allocating text memory. Failure leaves displayed text intact.
    pub fn set_text_fmt(self, args: fmt::Arguments<'_>) -> Result<()> {
        let _guard = self.storage.enter()?;
        self.check()?;
        let index = 1 - self.storage.active.load(Ordering::Relaxed);
        self.storage.format(index, args)?;
        self.check()?;
        // SAFETY: token validation proves the widget still owns this storage;
        // busy protects it until LVGL has consumed the new static pointer.
        unsafe {
            bindings::lv_label_set_text_static(
                self.label.raw(),
                self.storage.buffers[index].get().cast(),
            );
        };
        self.storage.active.store(index, Ordering::Relaxed);
        self.check()
    }

    pub fn set_text(self, text: &str) -> Result<()> {
        self.set_text_fmt(format_args!("{text}"))
    }
}

/// A noncapturing formatter for a reactive integer label.
pub type TextFormatter = fn(i32, &mut dyn fmt::Write) -> fmt::Result;

#[repr(C)]
struct Binding {
    header: bindings::ove_lvgl_text_binding_t,
    formatter: TextFormatter,
}

unsafe extern "C" fn render(
    obj: *mut c_void,
    cookie: u32,
    value: i32,
    context: *mut c_void,
) -> i32 {
    // SAFETY: copy the function before calling user code; user code can free
    // the observer/context, so no context reference survives the call.
    let formatter = unsafe { (*context.cast::<Binding>()).formatter };
    let mut buffer = TextBuffer::new();
    let result = (|| {
        formatter(value, &mut buffer).map_err(|_| buffer.error.unwrap_or(Error::InvalidParam))?;
        if let Some(error) = buffer.error {
            return Err(error);
        }
        // SAFETY: the C helper re-discovers the current observer and checks its
        // cookie and subject value before touching a possibly deleted target.
        Error::from_code(unsafe {
            bindings::ove_lvgl_text_publish(obj, cookie, value, buffer.as_ptr())
        })
    })();
    result.map_or_else(Error::to_code, |()| 0)
}

pub(super) fn bind(
    label: Label,
    subject: *mut bindings::lv_subject_t,
    formatter: TextFormatter,
) -> Result<()> {
    // SAFETY: LVGL owns and frees this plain context after registration. It has
    // no Rust drop fields; failure paths also release the allocation in C.
    unsafe {
        let context =
            bindings::ove_lvgl_text_alloc(core::mem::size_of::<Binding>()).cast::<Binding>();
        if context.is_null() {
            return Err(Error::NoMemory);
        }
        context.write(Binding {
            header: bindings::ove_lvgl_text_binding_t {
                render: Some(render),
                cookie: 0,
                status: 0,
            },
            formatter,
        });
        Error::from_code(bindings::ove_lvgl_text_bind(
            label.raw().cast(),
            subject.cast(),
            context.cast(),
        ))
    }
}
