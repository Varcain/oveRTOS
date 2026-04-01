/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_fs File System
 * @ingroup ove_data
 * @brief VFS abstraction for portable file and directory access.
 *
 * Provides a thin virtual file system (VFS) layer that maps POSIX-like
 * file and directory operations onto the underlying RTOS storage backend.
 * Volumes are mounted by path prefix, and all subsequent operations use
 * absolute path strings.
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
#define OVE_FS_O_READ   0x01
/** @brief Open for writing. */
#define OVE_FS_O_WRITE  0x02
/** @brief Create the file if it does not exist. */
#define OVE_FS_O_CREATE 0x04
/** @brief Seek to the end of the file before each write. */
#define OVE_FS_O_APPEND 0x08
/** @} */

/**
 * @name Seek whence constants
 * Passed as the @p whence argument to @ref ove_fs_seek.
 * @{
 */
/** @brief Seek relative to the beginning of the file. */
#define OVE_FS_SEEK_SET  0
/** @brief Seek relative to the current file position. */
#define OVE_FS_SEEK_CUR  1
/** @brief Seek relative to the end of the file. */
#define OVE_FS_SEEK_END  2
/** @} */

/**
 * @brief Directory entry descriptor returned by @ref ove_fs_readdir.
 */
struct ove_dirent {
	char name[256];   /**< @brief Null-terminated entry name (not full path). */
	unsigned int size; /**< @brief File size in bytes; 0 for directories. */
	int is_dir;       /**< @brief Non-zero if the entry is a directory. */
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
int  ove_fs_open_init(ove_file_t *file, ove_file_storage_t *storage,
		      const char *path, int flags);

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
int  ove_fs_close_deinit(ove_file_t file);

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
int  ove_fs_opendir_init(ove_dir_t *dir, ove_dir_storage_t *storage,
			 const char *path);

/**
 * @brief Close a statically-allocated directory handle.
 *
 * Releases the RTOS directory resources. The storage memory is not freed.
 *
 * @param[in] dir  Directory handle returned by @ref ove_fs_opendir_init.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_FS.
 */
int  ove_fs_closedir_deinit(ove_dir_t dir);

/**
 * @brief Open a file.
 *
 * Opens the file at @p path with the given @p flags. The returned handle
 * must be closed with @ref ove_fs_close when no longer needed.
 * In zero-heap mode, the backend uses a static pool instead of malloc.
 *
 * @param[out] file   Receives the opened file handle.
 * @param[in]  path   Absolute path of the file to open.
 * @param[in]  flags  Combination of @c OVE_FS_O_* flags.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_FS.
 */
int  ove_fs_open(ove_file_t *file, const char *path, int flags);

/**
 * @brief Close a file handle returned by @ref ove_fs_open.
 */
int  ove_fs_close(ove_file_t file);

/**
 * @brief Open a directory (heap or backend-managed allocation).
 */
int  ove_fs_opendir(ove_dir_t *dir, const char *path);

/**
 * @brief Close a directory handle returned by @ref ove_fs_opendir.
 */
int  ove_fs_closedir(ove_dir_t dir);

/**
 * @brief Mount a storage device at a virtual path prefix.
 *
 * Associates the block device at @p dev_path with the mount point
 * @p mount_point. All file and directory paths rooted at @p mount_point
 * will be dispatched to this device.
 *
 * @param[in] dev_path     Path identifying the storage device.
 * @param[in] mount_point  Absolute path to use as the mount prefix.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_fs_mount(const char *dev_path, const char *mount_point);

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
int  ove_fs_read(ove_file_t file, void *buf, size_t count,
		 size_t *bytes_read);

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
int  ove_fs_write(ove_file_t file, const void *buf, size_t count,
		  size_t *bytes_written);

/**
 * @brief Query the total size of an open file.
 *
 * @param[in]  file      Open file handle.
 * @param[out] out_size  Receives the file size in bytes.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_fs_size(ove_file_t file, size_t *out_size);

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
int  ove_fs_seek(ove_file_t file, long offset, int whence);

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
int  ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry);

/**
 * @brief Delete a file by path.
 *
 * Removes the file at @p path from the file system. The file must not
 * currently be open.
 *
 * @param[in] path  Absolute path of the file to delete.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_fs_unlink(const char *path);

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
int  ove_fs_rename(const char *old_path, const char *new_path);

#else /* !CONFIG_OVE_FS */

static inline int ove_fs_mount(const char *dev_path, const char *mount_point) { (void)dev_path; (void)mount_point; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_fs_unmount(const char *mount_point) { (void)mount_point; }
static inline int ove_fs_open(ove_file_t *file, const char *path, int flags) { (void)file; (void)path; (void)flags; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_close(ove_file_t file) { (void)file; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_read(ove_file_t file, void *buf, size_t count, size_t *bytes_read) { (void)file; (void)buf; (void)count; (void)bytes_read; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_write(ove_file_t file, const void *buf, size_t count, size_t *bytes_written) { (void)file; (void)buf; (void)count; (void)bytes_written; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_size(ove_file_t file, size_t *out_size) { (void)file; (void)out_size; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_seek(ove_file_t file, long offset, int whence) { (void)file; (void)offset; (void)whence; return OVE_ERR_NOT_SUPPORTED; }
static inline long ove_fs_tell(ove_file_t file) { (void)file; return -1; }
static inline int ove_fs_opendir(ove_dir_t *dir, const char *path) { (void)dir; (void)path; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry) { (void)dir; (void)entry; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_closedir(ove_dir_t dir) { (void)dir; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_unlink(const char *path) { (void)path; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_fs_rename(const char *old_path, const char *new_path) { (void)old_path; (void)new_path; return OVE_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_OVE_FS */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_fs group */

#endif /* OVE_FS_H */
