/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/fs.h"
#include "ove_backend_common.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>

#define SD_MOUNT_POINT "/mnt/sd"
#define SD_DEVICE "/dev/mmcsd0"

static void build_path(char *buf, size_t bufsz, const char *path)
{
	if (path[0] == '/') {
		snprintf(buf, bufsz, "%s%s", SD_MOUNT_POINT, path);
	} else {
		snprintf(buf, bufsz, "%s/%s", SD_MOUNT_POINT, path);
	}
}

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	(void)dev_path;
	(void)mount_point;

	int res = mount(SD_DEVICE, SD_MOUNT_POINT, "vfat", 0, NULL);
	if (res != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

void ove_fs_unmount(const char *mount_point)
{
	(void)mount_point;
	umount(SD_MOUNT_POINT);
}

int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	struct ove_file *f;
	char fullpath[128];
	int oflags = O_RDONLY;

	if (flags & OVE_FS_O_WRITE) {
		oflags = O_WRONLY;
		if (flags & OVE_FS_O_READ) {
			oflags = O_RDWR;
		}
	}
	if (flags & OVE_FS_O_CREATE) {
		oflags |= O_CREAT;
	}
	if (flags & OVE_FS_O_APPEND) {
		oflags |= O_APPEND;
	}

	f = OVE_BACKEND_MALLOC(sizeof(*f));
	if (f == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	build_path(fullpath, sizeof(fullpath), path);
	f->fd = open(fullpath, oflags);
	if (f->fd < 0) {
		OVE_BACKEND_FREE(f);
		return OVE_ERR_NOT_SUPPORTED;
	}

	*file = f;
	return OVE_OK;
}

int ove_fs_close(ove_file_t file)
{
	struct ove_file *f = file;
	close(f->fd);
	OVE_BACKEND_FREE(f);
	return OVE_OK;
}

int ove_fs_read(ove_file_t file, void *buf, size_t count,
			 size_t *bytes_read)
{
	struct ove_file *f = file;
	ssize_t br = read(f->fd, buf, count);
	if (br < 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (bytes_read != NULL) {
		*bytes_read = (size_t)br;
	}
	return OVE_OK;
}

int ove_fs_write(ove_file_t file, const void *buf,
			  size_t count, size_t *bytes_written)
{
	struct ove_file *f = file;
	ssize_t bw = write(f->fd, buf, count);
	if (bw < 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (bytes_written != NULL) {
		*bytes_written = (size_t)bw;
	}
	return OVE_OK;
}

int ove_fs_size(ove_file_t file, size_t *out_size)
{
	struct ove_file *f = file;
	struct stat st;
	if (fstat(f->fd, &st) != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	*out_size = st.st_size;
	return OVE_OK;
}

int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	struct ove_dir *d;
	char fullpath[128];

	d = OVE_BACKEND_MALLOC(sizeof(*d));
	if (d == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	if (path[0] == '/' && path[1] == '\0') {
		strncpy(fullpath, SD_MOUNT_POINT, sizeof(fullpath));
	} else {
		build_path(fullpath, sizeof(fullpath), path);
	}

	d->dp = opendir(fullpath);
	if (d->dp == NULL) {
		OVE_BACKEND_FREE(d);
		return OVE_ERR_NOT_SUPPORTED;
	}

	*dir = d;
	return OVE_OK;
}

int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	struct ove_dir *d = dir;
	struct dirent *ent;

	ent = readdir(d->dp);
	if (ent == NULL) {
		entry->name[0] = '\0';
		return OVE_OK;
	}

	strncpy(entry->name, ent->d_name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';

	/* Get file size via stat */
	char fullpath[128];
	snprintf(fullpath, sizeof(fullpath), "%s/%s",
		 SD_MOUNT_POINT, ent->d_name);
	struct stat st;
	if (stat(fullpath, &st) == 0) {
		entry->size = (unsigned int)st.st_size;
		entry->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
	} else {
		entry->size = 0;
		entry->is_dir = 0;
	}

	return OVE_OK;
}

int ove_fs_closedir(ove_dir_t dir)
{
	struct ove_dir *d = dir;
	closedir(d->dp);
	OVE_BACKEND_FREE(d);
	return OVE_OK;
}

int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	struct ove_file *f = file;
	int posix_whence;

	switch (whence) {
	case OVE_FS_SEEK_SET: posix_whence = SEEK_SET; break;
	case OVE_FS_SEEK_CUR: posix_whence = SEEK_CUR; break;
	case OVE_FS_SEEK_END: posix_whence = SEEK_END; break;
	default: return OVE_ERR_INVALID_PARAM;
	}

	off_t res = lseek(f->fd, (off_t)offset, posix_whence);
	if (res < 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

long ove_fs_tell(ove_file_t file)
{
	struct ove_file *f = file;
	return (long)lseek(f->fd, 0, SEEK_CUR);
}

int ove_fs_unlink(const char *path)
{
	char fullpath[128];
	build_path(fullpath, sizeof(fullpath), path);
	if (unlink(fullpath) != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	char old_full[128], new_full[128];
	build_path(old_full, sizeof(old_full), old_path);
	build_path(new_full, sizeof(new_full), new_path);
	if (rename(old_full, new_full) != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}
