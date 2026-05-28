/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file fs.hpp
 * @brief RAII file and directory handles with VFS operations
 */

#pragma once

#ifdef CONFIG_OVE_FS

#include <ove/fs.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

namespace ove
{

/**
 * @namespace ove::fs
 * @brief Thin C++ wrappers around the oveRTOS filesystem utility functions.
 *
 * Available when `CONFIG_OVE_FS` is enabled.  File and directory I/O is
 * handled by the `File` and `Dir` RAII classes below.
 */
namespace fs
{

/**
 * @brief Mounts a filesystem at the given mount point.
 * @param[in] dev_path    Path to the block device or image file.
 * @param[in] mount_point Directory path at which to mount the filesystem.
 * @return Empty `Result<void>` on success; `unexpected`
 *         @ref Error on failure.
 */
[[nodiscard]] inline Result<void> mount(const char *dev_path, const char *mount_point) noexcept
{
	return from_rc(ove_fs_mount(dev_path, mount_point));
}

/**
 * @brief Unmounts a previously mounted filesystem.
 * @param[in] mount_point The mount point path passed to `mount()`.
 */
inline void unmount(const char *mount_point)
{
	ove_fs_unmount(mount_point);
}

/**
 * @brief Deletes a file from the filesystem.
 * @param[in] path Absolute path to the file.
 * @return Empty `Result<void>` on success; `unexpected`
 *         @ref Error on failure (`Error::NotFound`, …).
 */
[[nodiscard]] inline Result<void> unlink(const char *path) noexcept
{
	return from_rc(ove_fs_unlink(path));
}

/**
 * @brief Renames or moves a file or directory.
 * @param[in] old_path Current path of the file or directory.
 * @param[in] new_path Desired new path.
 * @return Empty `Result<void>` on success; `unexpected`
 *         @ref Error on failure.
 */
[[nodiscard]] inline Result<void> rename(const char *old_path, const char *new_path) noexcept
{
	return from_rc(ove_fs_rename(old_path, new_path));
}

} /* namespace fs */

/**
 * @class File
 * @brief RAII wrapper around an oveRTOS file handle.
 *
 * The file is opened explicitly via `open()` and closed either by calling
 * `close()` or automatically by the destructor.  If the object is destroyed
 * with an open file, the destructor calls `close()`.
 *
 * @note Non-copyable; movable.
 */
class File
{
      public:
	/**
	 * @brief Constructs a File object with no open file (invalid state).
	 */
	File() : handle_(nullptr)
	{
	}

	/**
	 * @brief Destroys the file object, closing the file if it is still open.
	 */
	~File() noexcept
	{
		close();
	}

	File(const File &) = delete;
	File &operator=(const File &) = delete;

	/**
	 * @brief Move constructor — transfers ownership of the file handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	File(File &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — closes the current file and takes ownership.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	File &operator=(File &&other) noexcept
	{
		if (this != &other) {
			close();
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}

	/**
	 * @brief Opens a file at the specified path.
	 * @param[in] path  Absolute path to the file.
	 * @param[in] flags Open flags (e.g., read-only, write, create).
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> open(const char *path, int flags) noexcept
	{
		return from_rc(ove_fs_open(&handle_, path, flags));
	}

	/**
	 * @brief Closes the file and invalidates the handle.
	 *
	 * Safe to call on an already-closed file (no-op).  Errors from
	 * the backend (e.g. flush failures) are intentionally discarded
	 * here — call this on a known-good handle if you need to surface
	 * such failures.  Matches the close-is-void shape used by
	 * @ref TcpSocket and @ref UdpSocket.
	 */
	void close() noexcept
	{
		if (handle_) {
			(void)ove_fs_close(handle_);
			handle_ = nullptr;
		}
	}

	/**
	 * @brief Reads bytes from the file at the current position.
	 * @param[out] buf   Buffer to receive the read data.
	 * @param[in]  count Maximum number of bytes to read.
	 * @return On success, the number of bytes actually read (may be
	 *         less than @p count near end-of-file).  On failure, an
	 *         `unexpected` @ref Error.
	 */
	[[nodiscard]] Result<size_t> read(void *buf, size_t count) noexcept
	{
		size_t bytes_read = 0;
		const int rc = ove_fs_read(handle_, buf, count, &bytes_read);
		return from_rc(rc, bytes_read);
	}

	/**
	 * @brief Writes bytes to the file at the current position.
	 * @param[in] buf   Pointer to the data to write.
	 * @param[in] count Number of bytes to write.
	 * @return On success, the number of bytes actually written.  On
	 *         failure, an `unexpected` @ref Error.
	 */
	[[nodiscard]] Result<size_t> write(const void *buf, size_t count) noexcept
	{
		size_t bytes_written = 0;
		const int rc = ove_fs_write(handle_, buf, count, &bytes_written);
		return from_rc(rc, bytes_written);
	}

	/**
	 * @brief Repositions the file offset.
	 * @param[in] offset Byte offset relative to `whence`.
	 * @param[in] whence Seek origin (`SEEK_SET`, `SEEK_CUR`, or `SEEK_END`).
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> seek(long offset, int whence) noexcept
	{
		return from_rc(ove_fs_seek(handle_, offset, whence));
	}

	/**
	 * @brief Returns the current file offset.
	 * @return The current byte offset from the start of the file, or -1 on error.
	 */
	long tell()
	{
		return ove_fs_tell(handle_);
	}

	/**
	 * @brief Returns the size of the file.
	 * @return On success, the file size in bytes.  On failure, an
	 *         `unexpected` @ref Error.
	 */
	[[nodiscard]] Result<size_t> size() noexcept
	{
		size_t out_size = 0;
		const int rc = ove_fs_size(handle_, &out_size);
		return from_rc(rc, out_size);
	}

	/**
	 * @brief Returns `true` if the file handle is valid (file is open).
	 * @return `true` when a file has been successfully opened.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS file handle.
	 * @return The opaque `ove_file_t` handle.
	 */
	ove_file_t handle() const
	{
		return handle_;
	}

      private:
	ove_file_t handle_ = nullptr;
};

/**
 * @class Dir
 * @brief RAII wrapper around an oveRTOS directory handle.
 *
 * The directory is opened explicitly via `open()` and closed either by
 * calling `close()` or automatically by the destructor.
 *
 * @note Non-copyable; movable.
 */
class Dir
{
      public:
	/**
	 * @brief Constructs a Dir object with no open directory (invalid state).
	 */
	Dir() : handle_(nullptr)
	{
	}

	/**
	 * @brief Destroys the Dir object, closing the directory if it is still open.
	 */
	~Dir() noexcept
	{
		close();
	}

	Dir(const Dir &) = delete;
	Dir &operator=(const Dir &) = delete;

	/**
	 * @brief Move constructor — transfers ownership of the directory handle.
	 * @param other The source; its handle is set to null after the move.
	 */
	Dir(Dir &&other) noexcept : handle_(other.handle_)
	{
		other.handle_ = nullptr;
	}

	/**
	 * @brief Move-assignment operator — closes the current directory and takes ownership.
	 * @param other The source; its handle is set to null after the move.
	 * @return Reference to this object.
	 */
	Dir &operator=(Dir &&other) noexcept
	{
		if (this != &other) {
			close();
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}

	/**
	 * @brief Opens a directory at the specified path.
	 * @param[in] path Absolute path to the directory.
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> open(const char *path) noexcept
	{
		return from_rc(ove_fs_opendir(&handle_, path));
	}

	/**
	 * @brief Closes the directory and invalidates the handle.
	 *
	 * Safe to call on an already-closed directory (no-op).  Matches
	 * @ref File::close — backend errors are discarded.
	 */
	void close() noexcept
	{
		if (handle_) {
			(void)ove_fs_closedir(handle_);
			handle_ = nullptr;
		}
	}

	/**
	 * @brief Reads the next entry from the directory.
	 *
	 * @param[out] entry Pointer to a dirent struct to receive the entry data.
	 * @return `true` if an entry was read into @p entry; `false` at
	 *         end-of-directory.  On error, returns `unexpected` with the
	 *         corresponding @ref Error variant.
	 */
	[[nodiscard]] Result<bool> readdir(struct ove_dirent *entry) noexcept
	{
		const int rc = ove_fs_readdir(handle_, entry);
		if (rc == OVE_OK)
			return true;
		if (rc == OVE_ERR_EOF)
			return false;
		return std::unexpected{static_cast<Error>(rc)};
	}

	/**
	 * @brief Returns `true` if the directory handle is valid.
	 * @return `true` when a directory has been successfully opened.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS directory handle.
	 * @return The opaque `ove_dir_t` handle.
	 */
	ove_dir_t handle() const
	{
		return handle_;
	}

      private:
	ove_dir_t handle_ = nullptr;
};

} /* namespace ove */

#endif /* CONFIG_OVE_FS */
