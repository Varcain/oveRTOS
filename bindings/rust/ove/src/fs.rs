// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Filesystem abstraction for oveRTOS.
//!
//! Provides RAII [`File`] and [`Dir`] handles for reading, writing, and
//! directory traversal.
//!
//! Ordinary path inputs accept strings and byte slices, including non-UTF8
//! directory names. Private stack staging permits 255 payload bytes and rejects
//! NUL with `InvalidParam`, or longer paths with `NameTooLong`, before native
//! effects. Selected `_cstr` methods preserve native extensions without staging.
//! `File::open_fmt` formats once into the same bound, without an allocator;
//! failed formatting never opens a partial path.
//!
use core::ffi::CStr;

use crate::bindings;
use crate::cstring::Buffer;
use core::fmt;

const PATH_CAPACITY: usize = bindings::OVE_FS_PATH_MAX as usize;
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
/// Open flag: truncate an existing file to zero bytes.
pub const O_TRUNC: i32 = bindings::OVE_FS_O_TRUNC as i32;
/// Open flag: with [`O_CREATE`], fail if the path already exists.
pub const O_EXCL: i32 = bindings::OVE_FS_O_EXCL as i32;

/// Seek origin for [`File::seek`]: the start of the file.
pub const SEEK_SET: i32 = bindings::OVE_FS_SEEK_SET as i32;
/// Seek origin for [`File::seek`]: the current position.
pub const SEEK_CUR: i32 = bindings::OVE_FS_SEEK_CUR as i32;
/// Seek origin for [`File::seek`]: the end of the file.
pub const SEEK_END: i32 = bindings::OVE_FS_SEEK_END as i32;

/// Portable metadata returned by [`stat`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Metadata {
    /// Object size in bytes; zero for directories.
    pub size: u64,
    /// Last modification time in Unix seconds, or zero if unavailable.
    pub mtime_sec: u64,
    /// Whether this object is a directory.
    pub is_dir: bool,
}

/// Mount the board's storage volume.
///
/// Pass `None` for both to mount the board's volume: the only form that means
/// the same on every engine. `Some` values are backend-native names (see
/// `ove_fs_mount()`). File paths are relative to the mounted volume.
/// Supplied names use a portable 255-byte envelope. NUL is `InvalidParam`;
/// overflow is `NameTooLong`. `None` and `Some("")` remain distinct native
/// arguments. Use [`mount_cstr`] for longer native extensions.
pub fn mount(dev: Option<&str>, mnt: Option<&str>) -> Result<()> {
    let dev = dev
        .map(|s| Buffer::<PATH_CAPACITY>::from_bytes(s.as_bytes()))
        .transpose()?;
    let mnt = mnt
        .map(|s| Buffer::<PATH_CAPACITY>::from_bytes(s.as_bytes()))
        .transpose()?;
    let rc = unsafe {
        bindings::ove_fs_mount(
            dev.as_ref().map_or(core::ptr::null(), Buffer::as_ptr),
            mnt.as_ref().map_or(core::ptr::null(), Buffer::as_ptr),
        )
    };
    Error::from_code(rc)
}

/// Backend-native mount options, including names outside the portable envelope.
pub fn mount_cstr(dev: Option<&CStr>, mnt: Option<&CStr>) -> Result<()> {
    Error::from_code(unsafe {
        bindings::ove_fs_mount(
            dev.map_or(core::ptr::null(), CStr::as_ptr),
            mnt.map_or(core::ptr::null(), CStr::as_ptr),
        )
    })
}

/// Unmount by portable native name.
///
/// Close every file/directory before unmounting. The optional name uses the
/// same 255-byte/NUL policy as [`mount`], preserving `None` versus empty.
/// `Ok(())` means validation and dispatch; the native void operation cannot
/// report operational failure. [`unmount_cstr`] preserves native extensions.
pub fn unmount(mnt: Option<&str>) -> Result<()> {
    let mnt = mnt
        .map(|s| Buffer::<PATH_CAPACITY>::from_bytes(s.as_bytes()))
        .transpose()?;
    unsafe { bindings::ove_fs_unmount(mnt.as_ref().map_or(core::ptr::null(), Buffer::as_ptr)) };
    Ok(())
}

/// Unmount a backend-native C-string name.
pub fn unmount_cstr(mnt: Option<&CStr>) {
    unsafe { bindings::ove_fs_unmount(mnt.map_or(core::ptr::null(), CStr::as_ptr)) };
}

/// RAII file handle.
pub struct File {
    handle: bindings::ove_file_t,
}

impl File {
    /// Open the file at `path`.
    pub fn open(path: impl AsRef<[u8]>, flags: i32) -> Result<Self> {
        let path = Buffer::<PATH_CAPACITY>::from_bytes(path.as_ref())?;
        Self::open_ptr(path.as_ptr(), flags)
    }

    /// Format a portable path once, without an allocator or C-string adapter.
    pub fn open_fmt(args: fmt::Arguments<'_>, flags: i32) -> Result<Self> {
        let path = Buffer::<PATH_CAPACITY>::formatted(args, PATH_CAPACITY - 1, Error::NameTooLong)?;
        Self::open_ptr(path.as_ptr(), flags)
    }

    /// Open a backend-native C-string path, preserving native length extensions.
    pub fn open_cstr(path: &CStr, flags: i32) -> Result<Self> {
        Self::open_ptr(path.as_ptr(), flags)
    }

    fn open_ptr(path: *const core::ffi::c_char, flags: i32) -> Result<Self> {
        let mut handle: bindings::ove_file_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_fs_open(&mut handle, path, flags) };
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

    /// Total size of the open file in bytes.
    pub fn size(&self) -> Result<usize> {
        let mut size: usize = 0;
        let rc = unsafe { bindings::ove_fs_size(self.handle, &mut size) };
        Error::from_code(rc)?;
        Ok(size)
    }

    /// Move the position to `offset` bytes relative to `whence`
    /// ([`SEEK_SET`], [`SEEK_CUR`] or [`SEEK_END`]).
    ///
    /// Returns [`Error::InvalidParam`] if `offset` does not fit the target's
    /// C `long`.
    pub fn seek(&self, offset: i64, whence: i32) -> Result<()> {
        let offset = core::ffi::c_long::try_from(offset).map_err(|_| Error::InvalidParam)?;
        let rc = unsafe { bindings::ove_fs_seek(self.handle, offset, whence) };
        Error::from_code(rc)
    }

    /// Current position, in bytes from the start of the file.
    pub fn tell(&self) -> Result<u64> {
        let pos = unsafe { bindings::ove_fs_tell(self.handle) };
        u64::try_from(pos).map_err(|_| Error::Io)
    }

    /// Resize the open file.
    pub fn truncate(&self, length: u64) -> Result<()> {
        let rc = unsafe { bindings::ove_fs_truncate(self.handle, length) };
        Error::from_code(rc)
    }

    /// Flush buffered data and metadata to storage.
    pub fn sync(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_fs_sync(self.handle) };
        Error::from_code(rc)
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
    /// Open the directory at `path`.
    pub fn open(path: impl AsRef<[u8]>) -> Result<Self> {
        let path = Buffer::<PATH_CAPACITY>::from_bytes(path.as_ref())?;
        Self::open_ptr(path.as_ptr())
    }

    /// Open a backend-native C-string directory path.
    pub fn open_cstr(path: &CStr) -> Result<Self> {
        Self::open_ptr(path.as_ptr())
    }

    fn open_ptr(path: *const core::ffi::c_char) -> Result<Self> {
        let mut handle: bindings::ove_dir_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_fs_opendir(&mut handle, path) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Read the next directory entry.
    ///
    /// Returns `Ok(None)` at end-of-directory — signalled either by the
    /// `OVE_ERR_EOF` code or (on some backends) an empty entry name. Any
    /// other negative code is surfaced as an `Err`, so a genuine I/O failure
    /// mid-iteration is not silently mistaken for end-of-directory.
    pub fn read_entry(&mut self) -> Result<Option<DirEntry>> {
        let mut entry: bindings::ove_dirent = unsafe { core::mem::zeroed() };
        let rc = unsafe { bindings::ove_fs_readdir(self.handle, &mut entry) };
        match Error::from_code(rc) {
            Ok(()) => {}
            Err(Error::Eof) => return Ok(None),
            Err(e) => return Err(e),
        }
        // Some backends signal end-of-directory with an empty name instead
        // of OVE_ERR_EOF.
        if entry.name[0] == 0 {
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

/// Query metadata for `path`.
pub fn stat(path: impl AsRef<[u8]>) -> Result<Metadata> {
    let path = Buffer::<PATH_CAPACITY>::from_bytes(path.as_ref())?;
    // SAFETY: private storage is terminated and alive throughout the call.
    stat_cstr(unsafe { CStr::from_ptr(path.as_ptr()) })
}

/// Backend-native C-string variant of `stat`.
pub fn stat_cstr(path: &CStr) -> Result<Metadata> {
    let mut raw: bindings::ove_fs_stat = unsafe { core::mem::zeroed() };
    let rc = unsafe { bindings::ove_fs_stat(path.as_ptr(), &mut raw) };
    Error::from_code(rc)?;
    Ok(Metadata {
        size: raw.size,
        mtime_sec: raw.mtime_sec,
        is_dir: raw.type_ == bindings::OVE_FS_TYPE_DIR,
    })
}

/// Create a directory.
pub fn mkdir(path: impl AsRef<[u8]>) -> Result<()> {
    let path = Buffer::<PATH_CAPACITY>::from_bytes(path.as_ref())?;
    // SAFETY: private storage is terminated and alive throughout the call.
    mkdir_cstr(unsafe { CStr::from_ptr(path.as_ptr()) })
}

/// Backend-native C-string variant of `mkdir`.
pub fn mkdir_cstr(path: &CStr) -> Result<()> {
    let rc = unsafe { bindings::ove_fs_mkdir(path.as_ptr()) };
    Error::from_code(rc)
}

/// Remove an empty directory.
pub fn rmdir(path: impl AsRef<[u8]>) -> Result<()> {
    let path = Buffer::<PATH_CAPACITY>::from_bytes(path.as_ref())?;
    // SAFETY: private storage is terminated and alive throughout the call.
    rmdir_cstr(unsafe { CStr::from_ptr(path.as_ptr()) })
}

/// Backend-native C-string variant of `rmdir`.
pub fn rmdir_cstr(path: &CStr) -> Result<()> {
    let rc = unsafe { bindings::ove_fs_rmdir(path.as_ptr()) };
    Error::from_code(rc)
}

/// Delete a file.
pub fn unlink(path: impl AsRef<[u8]>) -> Result<()> {
    let path = Buffer::<PATH_CAPACITY>::from_bytes(path.as_ref())?;
    // SAFETY: private storage is terminated and alive throughout the call.
    unlink_cstr(unsafe { CStr::from_ptr(path.as_ptr()) })
}

/// Backend-native C-string variant of `unlink`.
pub fn unlink_cstr(path: &CStr) -> Result<()> {
    let rc = unsafe { bindings::ove_fs_unlink(path.as_ptr()) };
    Error::from_code(rc)
}

/// Rename a file or directory.
pub fn rename(old_path: impl AsRef<[u8]>, new_path: impl AsRef<[u8]>) -> Result<()> {
    let old = Buffer::<PATH_CAPACITY>::from_bytes(old_path.as_ref())?;
    let new = Buffer::<PATH_CAPACITY>::from_bytes(new_path.as_ref())?;
    Error::from_code(unsafe { bindings::ove_fs_rename(old.as_ptr(), new.as_ptr()) })
}

/// Backend-native C-string rename; both names are passed whole to the backend.
pub fn rename_cstr(old_path: &CStr, new_path: &CStr) -> Result<()> {
    let rc = unsafe { bindings::ove_fs_rename(old_path.as_ptr(), new_path.as_ptr()) };
    Error::from_code(rc)
}
