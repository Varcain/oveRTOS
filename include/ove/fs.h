/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file fs.h
 * @defgroup ove_fs File System
 * @ingroup ove_data
 * @brief VFS abstraction for portable file and directory access.
 *
 * Provides a thin file-system layer that maps POSIX-like file and directory
 * operations onto one mounted backend volume. Paths passed after mounting are
 * volume-relative; a leading slash denotes the root of that volume rather than
 * an oveRTOS-wide VFS namespace.
 *
 * File and directory handles follow the same dual-allocation pattern used
 * elsewhere in oveRTOS:
 * - **Static storage** via @c ove_fs_open_init / @c ove_fs_close_deinit
 *   and @c ove_fs_opendir_init / @c ove_fs_closedir_deinit.
 * - **Heap** via @ref ove_fs_open / @ref ove_fs_close and
 *   @ref ove_fs_opendir / @ref ove_fs_closedir — available only when
 *   @c OVE_HEAP_FS is defined.
 *
 * @note Requires @c CONFIG_OVE_FS.
 * @{
 */

#ifndef OVE_FS_H
#define OVE_FS_H

#include "ove/types.h"
#include "ove_config.h"
#include "ove/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name File open flags
 * Flags passed to @ref ove_fs_open or @ref ove_fs_open_init. Flags may be
 * combined with bitwise OR.
 * @{
 */
/** @brief Open for reading. */
#define OVE_FS_O_READ 0x01
/** @brief Open for writing. */
#define OVE_FS_O_WRITE 0x02
/** @brief Create the file if it does not exist. */
#define OVE_FS_O_CREATE 0x04
/** @brief Seek to the end of the file before each write. */
#define OVE_FS_O_APPEND 0x08
/** @brief Truncate an existing file to zero bytes when opening it. */
#define OVE_FS_O_TRUNC 0x10
/** @brief With @c OVE_FS_O_CREATE, fail if the path already exists. */
#define OVE_FS_O_EXCL 0x20
/** @} */

/** @brief Maximum supported path length including the null terminator. */
#define OVE_FS_PATH_MAX 256

/**
 * @name Seek whence constants
 * Passed as the @p whence argument to @ref ove_fs_seek.
 * @{
 */
/** @brief Seek relative to the beginning of the file. */
#define OVE_FS_SEEK_SET 0
/** @brief Seek relative to the current file position. */
#define OVE_FS_SEEK_CUR 1
/** @brief Seek relative to the end of the file. */
#define OVE_FS_SEEK_END 2
/** @} */

/**
 * @brief Directory entry descriptor returned by @ref ove_fs_readdir.
 */
struct ove_dirent {
	char name[256];	   /**< @brief Null-terminated entry name (not full path). */
	unsigned int size; /**< @brief File size in bytes; 0 for directories. */
	int is_dir;	   /**< @brief Non-zero if the entry is a directory. */
};

/** @brief Portable file-system object types returned by @ref ove_fs_stat. */
#define OVE_FS_TYPE_FILE 1u
#define OVE_FS_TYPE_DIR 2u

/** @brief Metadata for a file-system path. */
struct ove_fs_stat {
	uint64_t size;	    /**< @brief Object size in bytes; zero for directories. */
	uint64_t mtime_sec; /**< @brief Last modification time in Unix seconds, if available. */
	unsigned int type;  /**< @brief One of @c OVE_FS_TYPE_FILE or @c OVE_FS_TYPE_DIR. */
};

/** Validated raw-media view used when mounting a partition-backed volume. */
struct ove_fs_volume {
	uint64_t first_block;
	uint64_t block_count;
	uint32_t logical_block_size;
	uint8_t partition;
	uint8_t _reserved[3];
};

#ifdef CONFIG_OVE_FS

/**
 * @brief Open a file using caller-provided static storage.
 *
 * Opens the file at @p path with the given @p flags and stores the resulting
 * handle in @p file. The caller must ensure @p storage remains valid for the
 * lifetime of the open file.
 *
 * @param[out] file     Receives the opened file handle.
 * @param[in]  storage  Pointer to statically-allocated file storage.
 * @param[in]  path     Absolute path of the file to open.
 * @param[in]  flags    Combination of @c OVE_FS_O_* flags.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_FS.
 */
int ove_fs_open_init(ove_file_t *file, ove_file_storage_t *storage, const char *path, int flags);

/**
 * @brief Close a statically-allocated file handle.
 *
 * Flushes any pending writes and releases the RTOS file resources. The
 * storage memory is not freed.
 *
 * @param[in] file  File handle returned by @ref ove_fs_open_init.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_FS.
 */
int ove_fs_close_deinit(ove_file_t file);

/**
 * @brief Open a directory using caller-provided static storage.
 *
 * Opens the directory at @p path and stores the resulting handle in @p dir.
 * Use @ref ove_fs_readdir to iterate entries and @ref ove_fs_closedir_deinit
 * to close.
 *
 * @param[out] dir      Receives the opened directory handle.
 * @param[in]  storage  Pointer to statically-allocated directory storage.
 * @param[in]  path     Absolute path of the directory to open.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_FS.
 */
int ove_fs_opendir_init(ove_dir_t *dir, ove_dir_storage_t *storage, const char *path);

/**
 * @brief Close a statically-allocated directory handle.
 *
 * Releases the RTOS directory resources. The storage memory is not freed.
 *
 * @param[in] dir  Directory handle returned by @ref ove_fs_opendir_init.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_FS.
 */
int ove_fs_closedir_deinit(ove_dir_t dir);

/**
 * @brief Open a file (heap-backed handle).
 *
 * Opens the file at @p path with the given @p flags. The returned handle
 * must be closed with @ref ove_fs_close when no longer needed.
 *
 * @param[out] file   Receives the opened file handle.
 * @param[in]  path   Absolute path of the file to open.
 * @param[in]  flags  Combination of @c OVE_FS_O_* flags.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_FS and (per-backend) @c OVE_HEAP_FS. In
 *       zero-heap mode use @ref ove_fs_open_init with caller-supplied
 *       storage instead.
 */
int ove_fs_open(ove_file_t *file, const char *path, int flags);

/**
 * @brief Close a file handle returned by @ref ove_fs_open.
 *
 * @param[in] file File handle to close.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_close(ove_file_t file);

/**
 * @brief Open a directory (heap-backed handle).
 *
 * @param[out] dir  Receives the opened directory handle.
 * @param[in]  path Absolute path of the directory.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_FS and (per-backend) @c OVE_HEAP_FS. In
 *       zero-heap mode use @ref ove_fs_opendir_init.
 */
int ove_fs_opendir(ove_dir_t *dir, const char *path);

/**
 * @brief Close a directory handle returned by @ref ove_fs_opendir.
 *
 * @param[in] dir Directory handle to close.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_closedir(ove_dir_t dir);

/**
 * @brief Mount the backend storage volume.
 *
 * Backends with a native VFS use @p dev_path and @p mount_point directly.
 * Backends with a fixed logical drive accept @c NULL to select their board
 * defaults. File API paths remain relative to the mounted volume.
 *
 * @param[in] dev_path     Path identifying the storage device.
 * @param[in] mount_point  Absolute path to use as the mount prefix.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_mount(const char *dev_path, const char *mount_point);

/** Mount a validated whole-disk or partition view. */
int ove_fs_mount_volume(const struct ove_fs_volume *volume, const char *mount_point);

/**
 * @brief Unmount a previously mounted storage device.
 *
 * Flushes any pending data and detaches the device associated with
 * @p mount_point. All file handles under this mount point must be closed
 * before calling this function.
 *
 * @param[in] mount_point  Mount point string passed to @ref ove_fs_mount.
 */
void ove_fs_unmount(const char *mount_point);

/**
 * @brief Read bytes from an open file.
 *
 * Reads up to @p count bytes starting at the current file position into
 * @p buf. The file position advances by the number of bytes actually read.
 *
 * @param[in]  file        Open file handle.
 * @param[out] buf         Buffer to receive the data.
 * @param[in]  count       Maximum number of bytes to read.
 * @param[out] bytes_read  Receives the number of bytes actually read, or
 *                         @c NULL if not needed.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_read(ove_file_t file, void *buf, size_t count, size_t *bytes_read);

/**
 * @brief Write bytes to an open file.
 *
 * Writes up to @p count bytes from @p buf at the current file position.
 * The file position advances by the number of bytes actually written.
 * If the file was opened with @c OVE_FS_O_APPEND the write position is
 * set to the end of file before each write.
 *
 * @param[in]  file           Open file handle.
 * @param[in]  buf            Data to write.
 * @param[in]  count          Number of bytes to write.
 * @param[out] bytes_written  Receives the number of bytes actually written,
 *                            or @c NULL if not needed.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_write(ove_file_t file, const void *buf, size_t count, size_t *bytes_written);

/**
 * @brief Query the total size of an open file.
 *
 * @param[in]  file      Open file handle.
 * @param[out] out_size  Receives the file size in bytes.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_size(ove_file_t file, size_t *out_size);

/**
 * @brief Reposition the file read/write offset.
 *
 * Moves the current position of @p file to @p offset bytes relative to
 * the position described by @p whence.
 *
 * @param[in] file    Open file handle.
 * @param[in] offset  Byte offset relative to @p whence.
 * @param[in] whence  One of @c OVE_FS_SEEK_SET, @c OVE_FS_SEEK_CUR, or
 *                    @c OVE_FS_SEEK_END.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_seek(ove_file_t file, long offset, int whence);

/**
 * @brief Return the current file position.
 *
 * @param[in] file  Open file handle.
 * @return Current byte offset from the start of the file, or -1 on error.
 */
long ove_fs_tell(ove_file_t file);

/**
 * @brief Read the next entry from an open directory.
 *
 * Fills @p entry with information about the next directory entry and
 * advances the internal iterator. Returns a specific error when no more
 * entries are available.
 *
 * @param[in]  dir    Open directory handle.
 * @param[out] entry  Pointer to a @ref ove_dirent structure to fill.
 * @return OVE_OK if an entry was read, @c OVE_ERR_EOF when the directory
 *         is exhausted, or another negative error code on failure.
 */
int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry);

/**
 * @brief Delete a file by path.
 *
 * Removes the file at @p path from the file system. The file must not
 * currently be open.
 *
 * @param[in] path  Absolute path of the file to delete.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_unlink(const char *path);

/**
 * @brief Rename or move a file.
 *
 * Renames the file or directory at @p old_path to @p new_path. Both paths
 * must reside on the same mounted volume.
 *
 * @param[in] old_path  Absolute path of the existing file or directory.
 * @param[in] new_path  Absolute path for the new name or location.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_fs_rename(const char *old_path, const char *new_path);

/** @brief Query metadata for a path without opening it. */
int ove_fs_stat(const char *path, struct ove_fs_stat *out_stat);

/** @brief Create a directory. */
int ove_fs_mkdir(const char *path);

/** @brief Remove an empty directory. */
int ove_fs_rmdir(const char *path);

/** @brief Resize an open file. */
int ove_fs_truncate(ove_file_t file, uint64_t length);

/** @brief Flush buffered file data and metadata to the storage device. */
int ove_fs_sync(ove_file_t file);

#else /* !CONFIG_OVE_FS */

static inline int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	(void)dev_path;
	(void)mount_point;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_mount_volume(const struct ove_fs_volume *volume,
				      const char *mount_point)
{
	(void)volume;
	(void)mount_point;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_fs_unmount(const char *mount_point)
{
	(void)mount_point;
}
static inline int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	(void)file;
	(void)path;
	(void)flags;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_close(ove_file_t file)
{
	(void)file;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_read(ove_file_t file, void *buf, size_t count, size_t *bytes_read)
{
	(void)file;
	(void)buf;
	(void)count;
	(void)bytes_read;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_write(ove_file_t file, const void *buf, size_t count,
			       size_t *bytes_written)
{
	(void)file;
	(void)buf;
	(void)count;
	(void)bytes_written;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_size(ove_file_t file, size_t *out_size)
{
	(void)file;
	(void)out_size;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline long ove_fs_tell(ove_file_t file)
{
	(void)file;
	return -1;
}
static inline int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	(void)dir;
	(void)path;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	(void)dir;
	(void)entry;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_closedir(ove_dir_t dir)
{
	(void)dir;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_unlink(const char *path)
{
	(void)path;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_rename(const char *old_path, const char *new_path)
{
	(void)old_path;
	(void)new_path;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_stat(const char *path, struct ove_fs_stat *out_stat)
{
	(void)path;
	(void)out_stat;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_mkdir(const char *path)
{
	(void)path;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_rmdir(const char *path)
{
	(void)path;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_truncate(ove_file_t file, uint64_t length)
{
	(void)file;
	(void)length;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_fs_sync(ove_file_t file)
{
	(void)file;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_FS */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_fs group */

#endif /* OVE_FS_H */
