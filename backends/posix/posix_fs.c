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
	return mode;
}

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	(void)dev_path;
	(void)mount_point;
	return OVE_OK;
}

void ove_fs_unmount(const char *mount_point)
{
	(void)mount_point;
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
	int mode = flags_to_posix(flags);
	f->fd = open(path, mode, 0666);
	if (f->fd < 0) {
		int err = errno;
		OVE_BACKEND_FREE(f);
		return ove_errno_to_ove(err);
	}
	*file = f;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_fs_close(ove_file_t file)
{
	struct ove_file *f = file;
	if (!f) {
		return OVE_ERR_INVALID_PARAM;
	}
	close(f->fd);
	OVE_BACKEND_FREE(f);
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_fs_read(ove_file_t file, void *buf, size_t count,
		    size_t *bytes_read)
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

int ove_fs_write(ove_file_t file, const void *buf, size_t count,
		     size_t *bytes_written)
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
	off_t cur = lseek(f->fd, 0, SEEK_CUR);
	off_t end = lseek(f->fd, 0, SEEK_END);
	lseek(f->fd, cur, SEEK_SET);
	*out_size = (size_t)end;
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
	d->dp = opendir(path);
	if (!d->dp) {
		int err = errno;
		OVE_BACKEND_FREE(d);
		return ove_errno_to_ove(err);
	}
	*dir = d;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	struct ove_dir *d = dir;
	if (!d || !entry) {
		return OVE_ERR_INVALID_PARAM;
	}
	errno = 0;
	struct dirent *de = readdir(d->dp);
	if (!de) {
		/* readdir() returns NULL for both EOF and error — disambiguate via errno */
		if (errno != 0)
			return ove_errno_to_ove(errno);
		/* End of directory — return OK with empty name */
		memset(entry, 0, sizeof(*entry));
		return OVE_OK;
	}
	strncpy(entry->name, de->d_name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	entry->is_dir = (de->d_type == DT_DIR);
	entry->size = 0;
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_fs_closedir(ove_dir_t dir)
{
	struct ove_dir *d = dir;
	if (!d) {
		return OVE_ERR_INVALID_PARAM;
	}
	closedir(d->dp);
	OVE_BACKEND_FREE(d);
	return OVE_OK;
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
