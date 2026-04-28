/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stub filesystem backend for testing.
 *
 * On QEMU ARM (bare-metal): all operations return OVE_ERR_NOT_SUPPORTED.
 * On host/sim (NuttX sim, Zephyr native_sim): wraps POSIX file I/O.
 */

#ifndef OVE_QEMU_ARM
#define _GNU_SOURCE
#endif

#include "ove/ove.h"

#if defined(OVE_QEMU_ARM) || !(defined(CONFIG_OVE_RTOS_POSIX) || defined(CONFIG_OVE_RTOS_NUTTX))
/* Bare-metal or backends without POSIX-compatible FS structs: all ops return NOT_SUPPORTED */

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
	(void)file;
	(void)path;
	(void)flags;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_fs_close(ove_file_t file)
{
	(void)file;
	return OVE_ERR_INVALID_PARAM;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_fs_read(ove_file_t file, void *buf, size_t count, size_t *bytes_read)
{
	(void)file;
	(void)buf;
	(void)count;
	(void)bytes_read;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_fs_write(ove_file_t file, const void *buf, size_t count, size_t *bytes_written)
{
	(void)file;
	(void)buf;
	(void)count;
	(void)bytes_written;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_fs_size(ove_file_t file, size_t *out_size)
{
	(void)file;
	(void)out_size;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return OVE_ERR_NOT_SUPPORTED;
}

long ove_fs_tell(ove_file_t file)
{
	(void)file;
	return -1;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	(void)dir;
	(void)path;
	return OVE_ERR_NOT_SUPPORTED;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	(void)dir;
	(void)entry;
	return OVE_ERR_NOT_SUPPORTED;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_fs_closedir(ove_dir_t dir)
{
	(void)dir;
	return OVE_ERR_NOT_SUPPORTED;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_fs_unlink(const char *path)
{
	(void)path;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	(void)old_path;
	(void)new_path;
	return OVE_ERR_NOT_SUPPORTED;
}

#else
/* Native/POSIX fallback for sim targets (POSIX and NuttX backends have compatible structs) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

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
	struct ove_file *f = malloc(sizeof(*f));
	if (!f) {
		return OVE_ERR_NO_MEMORY;
	}
	int mode = flags_to_posix(flags);
	f->fd = open(path, mode, 0666);
	if (f->fd < 0) {
		free(f);
		return OVE_ERR_NOT_SUPPORTED;
	}
	*file = f;
	return OVE_OK;
}

int ove_fs_close(ove_file_t file)
{
	struct ove_file *f = file;
	if (!f) {
		return OVE_ERR_INVALID_PARAM;
	}
	close(f->fd);
	free(f);
	return OVE_OK;
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
		return OVE_ERR_NOT_SUPPORTED;
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
		return OVE_ERR_NOT_SUPPORTED;
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
		return OVE_ERR_NOT_SUPPORTED;
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
	struct ove_dir *d = malloc(sizeof(*d));
	if (!d) {
		return OVE_ERR_NO_MEMORY;
	}
	d->dp = opendir(path);
	if (!d->dp) {
		free(d);
		return OVE_ERR_NOT_SUPPORTED;
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
	struct dirent *de = readdir(d->dp);
	if (!de) {
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
	free(d);
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_fs_unlink(const char *path)
{
	if (!path) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (unlink(path) != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	if (!old_path || !new_path) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (rename(old_path, new_path) != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

#endif
