// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Virtual filesystem — `File` with open/read/write/seek/close and `Dir` for
//! directory enumeration, plus `mount`/`unmount`/`unlink`/`rename` helpers.
//!
//! Wraps `ove/fs.h`. Backed by `littlefs` on FreeRTOS, Zephyr's `fs_*` API on
//! Zephyr, the host POSIX VFS on POSIX, and NuttX's VFS on NuttX. Available
//! when `CONFIG_OVE_FS` is enabled.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Open flag: open for reading.
pub const O_READ = c.OVE_FS_O_READ;
/// Open flag: open for writing.
pub const O_WRITE = c.OVE_FS_O_WRITE;
/// Open flag: create the file if it does not exist.
pub const O_CREATE = c.OVE_FS_O_CREATE;
/// Open flag: append writes to the end of the file.
pub const O_APPEND = c.OVE_FS_O_APPEND;

/// Seek origin: seek relative to the start of the file.
pub const SEEK_SET = c.OVE_FS_SEEK_SET;
/// Seek origin: seek relative to the current file position.
pub const SEEK_CUR = c.OVE_FS_SEEK_CUR;
/// Seek origin: seek relative to the end of the file.
pub const SEEK_END = c.OVE_FS_SEEK_END;

const DIRENT_NAME_LEN = @typeInfo(@TypeOf(@as(c.struct_ove_dirent, undefined).name)).array.len;

/// A single directory entry returned by `Dir.readEntry()`.
pub const Dirent = struct {
    /// Null-terminated entry name (file or subdirectory name, not full path).
    name: [DIRENT_NAME_LEN]u8,
    /// `true` if this entry is a directory, `false` if it is a file.
    is_dir: bool,
    /// File size in bytes (0 for directories).
    size: usize,
};

/// File handle for reading, writing, and seeking within a file.
pub const File = struct {
    handle: c.ove_file_t,

    /// Open a file at `path` with the given `flags` (combination of `O_*` constants).
    ///
    /// Returns a `File` handle on success, or `Error` if the file cannot be opened.
    pub fn open(path: [*:0]const u8, flags: c_int) Error!File {
        var h: c.ove_file_t = null;
        try err.fromCode(c.ove_fs_open(&h, path, flags));
        return .{ .handle = h };
    }

    /// Close the file and release associated resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-closed file.
    pub fn close(self: *File) void {
        if (self.handle != null) {
            _ = c.ove_fs_close(self.handle);
            self.handle = null;
        }
    }

    /// Read up to `buf.len` bytes from the current file position into `buf`.
    ///
    /// Returns the number of bytes actually read. A return value of 0 indicates
    /// end-of-file. Returns `Error` on I/O failure.
    pub fn read(self: File, buf: []u8) Error!usize {
        var bytes_read: usize = 0;
        try err.fromCode(c.ove_fs_read(self.handle, buf.ptr, buf.len, &bytes_read));
        return bytes_read;
    }

    /// Write `buf` to the file at the current position.
    ///
    /// Returns the number of bytes actually written. Returns `Error` on I/O failure
    /// or if the filesystem is out of space.
    pub fn write(self: File, buf: []const u8) Error!usize {
        var bytes_written: usize = 0;
        try err.fromCode(c.ove_fs_write(self.handle, buf.ptr, buf.len, &bytes_written));
        return bytes_written;
    }

    /// Return the total size of this file in bytes.
    ///
    /// Returns `Error` on I/O failure.
    pub fn size(self: File) Error!usize {
        var out: usize = 0;
        try err.fromCode(c.ove_fs_size(self.handle, &out));
        return out;
    }

    /// Move the file position to `offset` bytes relative to `whence`.
    ///
    /// `whence` is one of `SEEK_SET`, `SEEK_CUR`, or `SEEK_END`.
    /// Returns `Error` if the seek position is invalid or the operation fails.
    pub fn seek(self: File, offset: i64, whence: c_int) Error!void {
        try err.fromCode(c.ove_fs_seek(self.handle, @intCast(offset), whence));
    }

    /// Return the current file position as a byte offset from the start.
    pub fn tell(self: File) i64 {
        return c.ove_fs_tell(self.handle);
    }

    // ----- std.io.GenericReader / GenericWriter integration -----

    const FileReader = std.io.GenericReader(File, Error, struct {
        fn read(self: File, buf: []u8) Error!usize {
            return File.read(self, buf);
        }
    }.read);

    const FileWriter = std.io.GenericWriter(File, Error, struct {
        fn write(self: File, buf: []const u8) Error!usize {
            return File.write(self, buf);
        }
    }.write);

    /// `std.io.GenericReader` view of this file.  `Reader.Error =
    /// ove.Error` since the substrate FS layer can fail at runtime
    /// (I/O errors, EOF signalled via `Ok(0)`).
    pub fn reader(self: File) FileReader {
        return .{ .context = self };
    }

    /// `std.io.GenericWriter` view of this file.
    pub fn writer(self: File) FileWriter {
        return .{ .context = self };
    }
};

/// Directory handle for iterating directory entries.
pub const Dir = struct {
    handle: c.ove_dir_t,

    /// Open the directory at `path`.
    ///
    /// Returns a `Dir` handle on success, or `Error` if the path does not exist
    /// or is not a directory.
    pub fn open(path: [*:0]const u8) Error!Dir {
        var h: c.ove_dir_t = null;
        try err.fromCode(c.ove_fs_opendir(&h, path));
        return .{ .handle = h };
    }

    /// Close the directory and release associated resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-closed directory.
    pub fn close(self: *Dir) void {
        if (self.handle != null) {
            _ = c.ove_fs_closedir(self.handle);
            self.handle = null;
        }
    }

    /// Read the next directory entry.
    ///
    /// Returns `null` when all entries have been enumerated (end of directory).
    /// Returns a `Dirent` describing the next entry, or `Error` on I/O failure.
    pub fn readEntry(self: Dir) Error!?Dirent {
        var raw_entry: c.struct_ove_dirent = undefined;
        try err.fromCode(c.ove_fs_readdir(self.handle, &raw_entry));
        // rc == 0 with empty name means end of directory
        if (raw_entry.name[0] == 0) return null;
        var entry: Dirent = .{
            .name = undefined,
            .is_dir = raw_entry.is_dir != 0,
            .size = raw_entry.size,
        };
        @memcpy(&entry.name, &raw_entry.name);
        return entry;
    }
};

/// Mount a filesystem at `mount_point`, backed by the block device at `dev_path`.
///
/// Returns `Error` if the mount fails (e.g. device not found, wrong filesystem type).
pub fn mount(dev_path: [*:0]const u8, mount_point: [*:0]const u8) Error!void {
    try err.fromCode(c.ove_fs_mount(dev_path, mount_point));
}

/// Unmount the filesystem at `mount_point`.
///
/// Flushes pending writes and releases the mount. The mount point becomes
/// inaccessible until `mount()` is called again.
pub fn unmount(mount_point: [*:0]const u8) void {
    c.ove_fs_unmount(mount_point);
}

/// Delete the file at `path`.
///
/// Returns `Error` if the file does not exist or cannot be deleted.
pub fn unlink(path: [*:0]const u8) Error!void {
    try err.fromCode(c.ove_fs_unlink(path));
}

/// Rename (or move) the file at `old_path` to `new_path`.
///
/// Returns `Error` if the source does not exist or the rename fails.
pub fn rename(old_path: [*:0]const u8, new_path: [*:0]const u8) Error!void {
    try err.fromCode(c.ove_fs_rename(old_path, new_path));
}
