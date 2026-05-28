// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Filesystem abstraction for oveRTOS.
//!
//! Provides RAII [`File`] and [`Dir`] handles for reading, writing, and
//! directory traversal. All paths must be null-terminated byte slices
//! (e.g. `b"/data/config.bin\0"`).

use crate::bindings;
use crate::error::{Error, Result};

// SAFETY (module-wide contract for the `unsafe { bindings::ove_*(...) }` FFI
// calls below): any handle passed to the C API is non-null and refers to a
// live RTOS object — wrapper constructors establish validity via
// `Error::from_code`, and `Drop` (or an explicit `deinit`) is the only place
// a handle is released. Pointer and slice arguments reference caller-owned
// memory valid for the duration of the call; the C side copies whatever it
// retains and does not alias them past return (verified against the
// signatures in `include/ove/*.h`). Blocks that deviate — `transmute`, raw
// pointer casts from user data, slice reconstruction via `from_raw_parts`,
// or storing a callback across the FFI boundary — carry their own
// `// SAFETY:` comment.

/// Open flag: open file for reading.
pub const O_READ: i32 = bindings::OVE_FS_O_READ as i32;
/// Open flag: open file for writing.
pub const O_WRITE: i32 = bindings::OVE_FS_O_WRITE as i32;
/// Open flag: create the file if it does not exist.
pub const O_CREATE: i32 = bindings::OVE_FS_O_CREATE as i32;
/// Open flag: all writes append to the end of the file.
pub const O_APPEND: i32 = bindings::OVE_FS_O_APPEND as i32;

/// Mount a filesystem.
///
/// Pass `None` for `dev` or `mnt` to use platform defaults.
pub fn mount(dev: Option<&[u8]>, mnt: Option<&[u8]>) -> Result<()> {
    let dev_ptr = dev.map_or(core::ptr::null(), |d| d.as_ptr() as *const _);
    let mnt_ptr = mnt.map_or(core::ptr::null(), |m| m.as_ptr() as *const _);
    let rc = unsafe { bindings::ove_fs_mount(dev_ptr, mnt_ptr) };
    Error::from_code(rc)
}

/// RAII file handle.
pub struct File {
    handle: bindings::ove_file_t,
}

impl File {
    /// Open a file. `path` must be `\0`-terminated.
    pub fn open(path: &[u8], flags: i32) -> Result<Self> {
        let mut handle: bindings::ove_file_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_fs_open(&mut handle, path.as_ptr() as *const _, flags) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Read from the file into `buf`. Returns the number of bytes read.
    pub fn read(&self, buf: &mut [u8]) -> Result<usize> {
        let mut bytes_read: usize = 0;
        let rc = unsafe {
            bindings::ove_fs_read(
                self.handle,
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                &mut bytes_read,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_read)
    }

    /// Write `buf` to the file. Returns the number of bytes written.
    pub fn write(&self, buf: &[u8]) -> Result<usize> {
        let mut bytes_written: usize = 0;
        let rc = unsafe {
            bindings::ove_fs_write(
                self.handle,
                buf.as_ptr() as *const _,
                buf.len(),
                &mut bytes_written,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_written)
    }
}

impl Drop for File {
    fn drop(&mut self) {
        unsafe { bindings::ove_fs_close(self.handle) };
    }
}

// SAFETY: File wraps an opaque RTOS handle. Access is single-threaded by
// application design (only one thread owns the File at a time).
unsafe impl Send for File {}

/// A directory entry returned by `Dir::read_entry`.
pub struct DirEntry {
    inner: bindings::ove_dirent,
}

impl DirEntry {
    /// The entry name as a byte slice (without trailing `\0`).
    pub fn name(&self) -> &[u8] {
        // SAFETY: `self.inner.name` is an inline fixed-size array owned by this
        // `DirEntry`; the slice borrows it for `&self`'s lifetime.
        let name_bytes = unsafe {
            core::slice::from_raw_parts(
                self.inner.name.as_ptr() as *const u8,
                self.inner.name.len(),
            )
        };
        cstr_to_slice(name_bytes)
    }

    /// The file size in bytes.
    pub fn size(&self) -> u32 {
        self.inner.size
    }

    /// Whether this entry is a directory.
    pub fn is_dir(&self) -> bool {
        self.inner.is_dir != 0
    }
}

/// RAII directory handle.
pub struct Dir {
    handle: bindings::ove_dir_t,
}

impl Dir {
    /// Open a directory. `path` must be `\0`-terminated.
    pub fn open(path: &[u8]) -> Result<Self> {
        let mut handle: bindings::ove_dir_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_fs_opendir(&mut handle, path.as_ptr() as *const _) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Read the next directory entry.
    ///
    /// Returns `Ok(None)` at end-of-directory. Different backends signal
    /// end-of-dir differently (error code vs. empty name), so both are
    /// treated as `Ok(None)`.
    pub fn read_entry(&mut self) -> Result<Option<DirEntry>> {
        let mut entry: bindings::ove_dirent = unsafe { core::mem::zeroed() };
        let rc = unsafe { bindings::ove_fs_readdir(self.handle, &mut entry) };
        if rc != 0 || entry.name[0] == 0 {
            return Ok(None);
        }
        Ok(Some(DirEntry { inner: entry }))
    }
}

impl Drop for Dir {
    fn drop(&mut self) {
        unsafe { bindings::ove_fs_closedir(self.handle) };
    }
}

// SAFETY: Dir wraps an opaque RTOS handle. Access is single-threaded by
// application design.
unsafe impl Send for Dir {}

fn cstr_to_slice(buf: &[u8]) -> &[u8] {
    let mut len = 0;
    while len < buf.len() && buf[len] != 0 {
        len += 1;
    }
    &buf[..len]
}
