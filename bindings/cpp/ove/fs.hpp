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
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int mount(const char *dev_path, const char *mount_point)
{
	return ove_fs_mount(dev_path, mount_point);
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
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int unlink(const char *path)
{
	return ove_fs_unlink(path);
}

/**
 * @brief Renames or moves a file or directory.
 * @param[in] old_path Current path of the file or directory.
 * @param[in] new_path Desired new path.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int rename(const char *old_path, const char *new_path)
{
	return ove_fs_rename(old_path, new_path);
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
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int open(const char *path, int flags)
	{
		return ove_fs_open(&handle_, path, flags);
	}

	/**
	 * @brief Closes the file and invalidates the handle.
	 *
	 * Safe to call on an already-closed file (returns `OVE_OK`).
	 *
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	int close()
	{
		int ret = OVE_OK;
		if (handle_) {
			ret = ove_fs_close(handle_);
			handle_ = nullptr;
		}
		return ret;
	}

	/**
	 * @brief Reads bytes from the file at the current position.
	 * @param[out] buf        Buffer to receive the read data.
	 * @param[in]  count      Maximum number of bytes to read.
	 * @param[out] bytes_read Receives the actual number of bytes read.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int read(void *buf, size_t count, size_t *bytes_read)
	{
		return ove_fs_read(handle_, buf, count, bytes_read);
	}

	/**
	 * @brief Writes bytes to the file at the current position.
	 * @param[in]  buf           Pointer to the data to write.
	 * @param[in]  count         Number of bytes to write.
	 * @param[out] bytes_written Receives the actual number of bytes written.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int write(const void *buf, size_t count, size_t *bytes_written)
	{
		return ove_fs_write(handle_, buf, count, bytes_written);
	}

	/**
	 * @brief Repositions the file offset.
	 * @param[in] offset Byte offset relative to `whence`.
	 * @param[in] whence Seek origin (`SEEK_SET`, `SEEK_CUR`, or `SEEK_END`).
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int seek(long offset, int whence)
	{
		return ove_fs_seek(handle_, offset, whence);
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
	 * @param[out] out_size Receives the file size in bytes.
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int size(size_t *out_size)
	{
		return ove_fs_size(handle_, out_size);
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
	ove_file_t handle_;
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
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	[[nodiscard]] int open(const char *path)
	{
		return ove_fs_opendir(&handle_, path);
	}

	/**
	 * @brief Closes the directory and invalidates the handle.
	 *
	 * Safe to call on an already-closed directory (returns `OVE_OK`).
	 *
	 * @return `OVE_OK` on success, or a negative error code.
	 */
	int close()
	{
		int ret = OVE_OK;
		if (handle_) {
			ret = ove_fs_closedir(handle_);
			handle_ = nullptr;
		}
		return ret;
	}

	/**
	 * @brief Reads the next entry from the directory.
	 * @param[out] entry Pointer to a dirent struct to receive the entry data.
	 * @return `OVE_OK` on success, a positive value at end-of-directory, or
	 *         a negative error code on failure.
	 */
	[[nodiscard]] int readdir(struct ove_dirent *entry)
	{
		return ove_fs_readdir(handle_, entry);
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
	ove_dir_t handle_;
};

} /* namespace ove */

#endif /* CONFIG_OVE_FS */
