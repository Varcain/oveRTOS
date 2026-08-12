/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#define _GNU_SOURCE
#include "ove/ove.h"
#include "ove_backend_common.h"
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

static int flags_to_posix(int flags)
{
	int mode = 0;
	if ((flags & OVE_FS_O_READ) && (flags & OVE_FS_O_WRITE)) {
		mode = O_RDWR;
	} else if (flags & OVE_FS_O_WRITE) {
		mode = O_WRONLY;
	} else {
		mode = O_RDONLY;
	}
	if (flags & OVE_FS_O_CREATE) {
		mode |= O_CREAT;
	}
	if (flags & OVE_FS_O_APPEND) {
		mode |= O_APPEND;
	}
	if (flags & OVE_FS_O_TRUNC) {
		mode |= O_TRUNC;
	}
	if (flags & OVE_FS_O_EXCL) {
		mode |= O_EXCL;
	}
	return mode;
}

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	(void)dev_path;
	(void)mount_point;
	return OVE_OK;
}

int ove_fs_mount_volume(const struct ove_fs_volume *volume, const char *mount_point)
{
	if (!volume || volume->block_count == 0u || volume->logical_block_size == 0u)
		return OVE_ERR_INVALID_PARAM;
	return ove_fs_mount(NULL, mount_point);
}

void ove_fs_unmount(const char *mount_point)
{
	(void)mount_point;
}

int ove_fs_open_init(ove_file_t *file, ove_file_storage_t *storage, const char *path, int flags)
{
	if (!file || !storage || !path) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_file *f = storage;
	int mode = flags_to_posix(flags);
	f->fd = open(path, mode, 0666);
	if (f->fd < 0) {
		return ove_errno_to_ove(errno);
	}
	*file = f;
	return OVE_OK;
}

int ove_fs_close_deinit(ove_file_t file)
{
	struct ove_file *f = file;
	if (!f) {
		return OVE_ERR_INVALID_PARAM;
	}
	return close(f->fd) == 0 ? OVE_OK : ove_errno_to_ove(errno);
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	if (!file || !path) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_file *f = OVE_BACKEND_MALLOC(sizeof(*f));
	if (!f) {
		return OVE_ERR_NO_MEMORY;
	}
	int rc = ove_fs_open_init(file, f, path, flags);
	if (rc != OVE_OK) {
		OVE_BACKEND_FREE(f);
	}
	return rc;
}

int ove_fs_close(ove_file_t file)
{
	int rc = ove_fs_close_deinit(file);
	if (rc == OVE_OK) {
		OVE_BACKEND_FREE(file);
	}
	return rc;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_fs_read(ove_file_t file, void *buf, size_t count, size_t *bytes_read)
{
	struct ove_file *f = file;
	if (!f || !buf) {
		return OVE_ERR_INVALID_PARAM;
	}
	ssize_t n = read(f->fd, buf, count);
	if (n < 0) {
		return ove_errno_to_ove(errno);
	}
	if (bytes_read) {
		*bytes_read = (size_t)n;
	}
	return OVE_OK;
}

int ove_fs_write(ove_file_t file, const void *buf, size_t count, size_t *bytes_written)
{
	struct ove_file *f = file;
	if (!f || !buf) {
		return OVE_ERR_INVALID_PARAM;
	}
	ssize_t n = write(f->fd, buf, count);
	if (n < 0) {
		return ove_errno_to_ove(errno);
	}
	if (bytes_written) {
		*bytes_written = (size_t)n;
	}
	return OVE_OK;
}

int ove_fs_size(ove_file_t file, size_t *out_size)
{
	struct ove_file *f = file;
	if (!f || !out_size) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct stat st;
	if (fstat(f->fd, &st) != 0) {
		return ove_errno_to_ove(errno);
	}
	*out_size = (size_t)st.st_size;
	return OVE_OK;
}

int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	struct ove_file *f = file;
	if (!f) {
		return OVE_ERR_INVALID_PARAM;
	}
	int w;
	switch (whence) {
	case OVE_FS_SEEK_SET:
		w = SEEK_SET;
		break;
	case OVE_FS_SEEK_CUR:
		w = SEEK_CUR;
		break;
	case OVE_FS_SEEK_END:
		w = SEEK_END;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}
	if (lseek(f->fd, offset, w) < 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

long ove_fs_tell(ove_file_t file)
{
	struct ove_file *f = file;
	if (!f) {
		return -1;
	}
	return (long)lseek(f->fd, 0, SEEK_CUR);
}

int ove_fs_opendir_init(ove_dir_t *dir, ove_dir_storage_t *storage, const char *path)
{
	if (!dir || !storage || !path) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_dir *d = storage;
	d->dp = opendir(path);
	if (!d->dp) {
		return ove_errno_to_ove(errno);
	}
	*dir = d;
	return OVE_OK;
}

int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	struct ove_dir *d = dir;
	if (!d || !entry) {
		return OVE_ERR_INVALID_PARAM;
	}
	errno = 0;
	/* POSIX readdir() is per-stream thread-safe in practice on glibc/musl
	 * (each ove_dir owns its own DIR*); readdir_r is deprecated. */
	struct dirent *de = readdir(d->dp); // NOLINT(concurrency-mt-unsafe)
	if (!de) {
		/* readdir() returns NULL for both EOF and error — disambiguate via errno */
		if (errno != 0)
			return ove_errno_to_ove(errno);
		memset(entry, 0, sizeof(*entry));
		return OVE_ERR_EOF;
	}
	strncpy(entry->name, de->d_name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	entry->is_dir = (de->d_type == DT_DIR);
	entry->size = 0;
	return OVE_OK;
}

int ove_fs_closedir_deinit(ove_dir_t dir)
{
	struct ove_dir *d = dir;
	if (!d) {
		return OVE_ERR_INVALID_PARAM;
	}
	return closedir(d->dp) == 0 ? OVE_OK : ove_errno_to_ove(errno);
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	if (!dir || !path) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_dir *d = OVE_BACKEND_MALLOC(sizeof(*d));
	if (!d) {
		return OVE_ERR_NO_MEMORY;
	}
	int rc = ove_fs_opendir_init(dir, d, path);
	if (rc != OVE_OK) {
		OVE_BACKEND_FREE(d);
	}
	return rc;
}

int ove_fs_closedir(ove_dir_t dir)
{
	int rc = ove_fs_closedir_deinit(dir);
	if (rc == OVE_OK) {
		OVE_BACKEND_FREE(dir);
	}
	return rc;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_fs_unlink(const char *path)
{
	if (!path) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (unlink(path) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	if (!old_path || !new_path) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (rename(old_path, new_path) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_stat(const char *path, struct ove_fs_stat *out_stat)
{
	struct stat st;

	if (!path || !out_stat) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (stat(path, &st) != 0) {
		return ove_errno_to_ove(errno);
	}
	out_stat->size = S_ISDIR(st.st_mode) ? 0u : (uint64_t)st.st_size;
	out_stat->mtime_sec = (uint64_t)st.st_mtime;
	out_stat->type = S_ISDIR(st.st_mode) ? OVE_FS_TYPE_DIR : OVE_FS_TYPE_FILE;
	return OVE_OK;
}

int ove_fs_mkdir(const char *path)
{
	if (!path) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (mkdir(path, 0777) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_rmdir(const char *path)
{
	if (!path) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (rmdir(path) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_truncate(ove_file_t file, uint64_t length)
{
	struct ove_file *f = file;

	if (!f || length > (uint64_t)INT64_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (ftruncate(f->fd, (off_t)length) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_sync(ove_file_t file)
{
	struct ove_file *f = file;

	if (!f) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (fsync(f->fd) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}
