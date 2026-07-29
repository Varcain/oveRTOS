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
#include <errno.h>
#define SD_MOUNT_POINT_DEFAULT "/mnt/sd"
#define SD_DEVICE_DEFAULT "/dev/mmcsd0"
#define NATIVE_PATH_MAX 320

static char active_mount_point[64] = SD_MOUNT_POINT_DEFAULT;
static int volume_mounted;

static int build_path(char *buf, size_t bufsz, const char *path)
{
	if (!volume_mounted) {
		return OVE_ERR_NOT_REGISTERED;
	}
	if (path == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (strnlen(path, OVE_FS_PATH_MAX) >= OVE_FS_PATH_MAX) {
		return OVE_ERR_NAME_TOO_LONG;
	}

	int needed;
	if (path[0] == '/' && path[1] == '\0') {
		needed = snprintf(buf, bufsz, "%s", active_mount_point);
	} else if (path[0] == '/') {
		needed = snprintf(buf, bufsz, "%s%s", active_mount_point, path);
	} else {
		needed = snprintf(buf, bufsz, "%s/%s", active_mount_point, path);
	}
	return needed < 0 || (size_t)needed >= bufsz ? OVE_ERR_NAME_TOO_LONG : OVE_OK;
}

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	const char *device = dev_path != NULL ? dev_path : SD_DEVICE_DEFAULT;
	const char *target = mount_point != NULL ? mount_point : SD_MOUNT_POINT_DEFAULT;

	if (strnlen(target, sizeof(active_mount_point)) >= sizeof(active_mount_point)) {
		return OVE_ERR_NAME_TOO_LONG;
	}
	if (volume_mounted) {
		return strcmp(target, active_mount_point) == 0 ? OVE_OK : OVE_ERR_BUSY;
	}

	if (strcmp(target, SD_MOUNT_POINT_DEFAULT) == 0) {
		if (mkdir("/mnt", 0777) != 0 && errno != EEXIST) {
			return ove_errno_to_ove(errno);
		}
	}
	if (mkdir(target, 0777) != 0 && errno != EEXIST) {
		return ove_errno_to_ove(errno);
	}

	int res = mount(device, target, "vfat", 0, NULL);
	if (res != 0) {
		return ove_errno_to_ove(errno);
	}
	strncpy(active_mount_point, target, sizeof(active_mount_point));
	active_mount_point[sizeof(active_mount_point) - 1] = '\0';
	volume_mounted = 1;
	return OVE_OK;
}

void ove_fs_unmount(const char *mount_point)
{
	if (!volume_mounted ||
	    (mount_point != NULL && strcmp(mount_point, active_mount_point) != 0)) {
		return;
	}
	if (umount(active_mount_point) == 0) {
		volume_mounted = 0;
	}
}

static int open_common(struct ove_file *f, const char *path, int flags)
{
	char fullpath[NATIVE_PATH_MAX];
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
	if (flags & OVE_FS_O_TRUNC) {
		oflags |= O_TRUNC;
	}
	if (flags & OVE_FS_O_EXCL) {
		oflags |= O_EXCL;
	}

	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
	}
	f->fd = open(fullpath, oflags, 0666);
	if (f->fd < 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

static int opendir_common(struct ove_dir *dir, const char *path)
{
	int ret = build_path(dir->path, sizeof(dir->path), path);
	if (ret != OVE_OK) {
		return ret;
	}
	dir->dp = opendir(dir->path);
	return dir->dp != NULL ? OVE_OK : ove_errno_to_ove(errno);
}

/* ─── _init / _deinit (static storage) ───────────────────────────────── */

int ove_fs_open_init(ove_file_t *file, ove_file_storage_t *storage, const char *path, int flags)
{
	if (file == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_file *f = (struct ove_file *)storage;
	int ret = open_common(f, path, flags);
	if (ret != OVE_OK) {
		return ret;
	}
	*file = f;
	return OVE_OK;
}

int ove_fs_close_deinit(ove_file_t file)
{
	if (file == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	return close(file->fd) == 0 ? OVE_OK : ove_errno_to_ove(errno);
}

int ove_fs_opendir_init(ove_dir_t *dir, ove_dir_storage_t *storage, const char *path)
{
	struct ove_dir *d = (struct ove_dir *)storage;
	if (dir == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = opendir_common(d, path);
	if (ret != OVE_OK) {
		return ret;
	}

	*dir = d;
	return OVE_OK;
}

int ove_fs_closedir_deinit(ove_dir_t dir)
{
	if (dir == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	return closedir(dir->dp) == 0 ? OVE_OK : ove_errno_to_ove(errno);
}

/* ─── _create / _destroy (heap or static pool) ──────────────────────── */

#ifdef OVE_HEAP_FS
int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	struct ove_file *f;

	f = OVE_BACKEND_MALLOC(sizeof(*f));
	if (f == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = open_common(f, path, flags);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(f);
		return ret;
	}

	*file = f;
	return OVE_OK;
}

int ove_fs_close(ove_file_t file)
{
	struct ove_file *f = file;
	int ret = ove_fs_close_deinit(f);
	if (ret == OVE_OK) {
		OVE_BACKEND_FREE(f);
	}
	return ret;
}
#else /* zero-heap: static pool */
#define FS_POOL_FILES 4
#define FS_POOL_DIRS 4
static struct ove_file file_pool[FS_POOL_FILES];
static int file_pool_used[FS_POOL_FILES];
static struct ove_dir dir_pool[FS_POOL_DIRS];
static int dir_pool_used[FS_POOL_DIRS];

int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	for (int i = 0; i < FS_POOL_FILES; i++) {
		if (!file_pool_used[i]) {
			file_pool_used[i] = 1;
			int ret = open_common(&file_pool[i], path, flags);
			if (ret != OVE_OK) {
				file_pool_used[i] = 0;
				return ret;
			}
			*file = &file_pool[i];
			return OVE_OK;
		}
	}
	return OVE_ERR_NO_MEMORY;
}

int ove_fs_close(ove_file_t file)
{
	int ret = ove_fs_close_deinit(file);
	if (ret != OVE_OK) {
		return ret;
	}
	for (int i = 0; i < FS_POOL_FILES; i++) {
		if (&file_pool[i] == file) {
			file_pool_used[i] = 0;
			break;
		}
	}
	return OVE_OK;
}
#endif /* OVE_HEAP_FS */

int ove_fs_read(ove_file_t file, void *buf, size_t count, size_t *bytes_read)
{
	struct ove_file *f = file;
	ssize_t br = read(f->fd, buf, count);
	if (br < 0) {
		return ove_errno_to_ove(errno);
	}
	if (bytes_read != NULL) {
		*bytes_read = (size_t)br;
	}
	return OVE_OK;
}

int ove_fs_write(ove_file_t file, const void *buf, size_t count, size_t *bytes_written)
{
	struct ove_file *f = file;
	ssize_t bw = write(f->fd, buf, count);
	if (bw < 0) {
		return ove_errno_to_ove(errno);
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
		return ove_errno_to_ove(errno);
	}
	*out_size = st.st_size;
	return OVE_OK;
}

#ifdef OVE_HEAP_FS
int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	struct ove_dir *d;

	d = OVE_BACKEND_MALLOC(sizeof(*d));
	if (d == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = opendir_common(d, path);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(d);
		return ret;
	}

	*dir = d;
	return OVE_OK;
}
#else  /* zero-heap: static pool */
int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	for (int i = 0; i < FS_POOL_DIRS; i++) {
		if (!dir_pool_used[i]) {
			dir_pool_used[i] = 1;
			int ret = opendir_common(&dir_pool[i], path);
			if (ret != OVE_OK) {
				dir_pool_used[i] = 0;
				return ret;
			}
			*dir = &dir_pool[i];
			return OVE_OK;
		}
	}
	return OVE_ERR_NO_MEMORY;
}
#endif /* OVE_HEAP_FS */

int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	if (dir == NULL || entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_dir *d = dir;
	struct dirent *ent;

	errno = 0;
	ent = readdir(d->dp);
	if (ent == NULL) {
		entry->name[0] = '\0';
		return (errno == 0) ? OVE_ERR_EOF : ove_errno_to_ove(errno);
	}

	strncpy(entry->name, ent->d_name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';

	/* Get file size via stat */
	char fullpath[NATIVE_PATH_MAX + OVE_FS_PATH_MAX];
	int needed = snprintf(fullpath, sizeof(fullpath), "%s/%s", d->path, ent->d_name);
	if (needed < 0 || (size_t)needed >= sizeof(fullpath)) {
		return OVE_ERR_NAME_TOO_LONG;
	}
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

#ifdef OVE_HEAP_FS
int ove_fs_closedir(ove_dir_t dir)
{
	struct ove_dir *d = dir;
	int ret = ove_fs_closedir_deinit(d);
	if (ret == OVE_OK) {
		OVE_BACKEND_FREE(d);
	}
	return ret;
}
#else  /* zero-heap: static pool */
int ove_fs_closedir(ove_dir_t dir)
{
	int ret = ove_fs_closedir_deinit(dir);
	if (ret != OVE_OK) {
		return ret;
	}
	for (int i = 0; i < FS_POOL_DIRS; i++) {
		if (&dir_pool[i] == dir) {
			dir_pool_used[i] = 0;
			break;
		}
	}
	return OVE_OK;
}
#endif /* OVE_HEAP_FS */

int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	struct ove_file *f = file;
	int posix_whence;

	switch (whence) {
	case OVE_FS_SEEK_SET:
		posix_whence = SEEK_SET;
		break;
	case OVE_FS_SEEK_CUR:
		posix_whence = SEEK_CUR;
		break;
	case OVE_FS_SEEK_END:
		posix_whence = SEEK_END;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}

	off_t res = lseek(f->fd, (off_t)offset, posix_whence);
	if (res < 0) {
		return ove_errno_to_ove(errno);
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
	char fullpath[NATIVE_PATH_MAX];
	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
	}
	if (unlink(fullpath) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	char old_full[NATIVE_PATH_MAX], new_full[NATIVE_PATH_MAX];
	int ret = build_path(old_full, sizeof(old_full), old_path);
	if (ret == OVE_OK) {
		ret = build_path(new_full, sizeof(new_full), new_path);
	}
	if (ret != OVE_OK) {
		return ret;
	}
	if (rename(old_full, new_full) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_stat(const char *path, struct ove_fs_stat *out_stat)
{
	char fullpath[NATIVE_PATH_MAX];
	struct stat st;

	if (path == NULL || out_stat == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
	}
	if (stat(fullpath, &st) != 0) {
		return ove_errno_to_ove(errno);
	}
	out_stat->size = S_ISDIR(st.st_mode) ? 0u : (uint64_t)st.st_size;
	out_stat->mtime_sec = (uint64_t)st.st_mtime;
	out_stat->type = S_ISDIR(st.st_mode) ? OVE_FS_TYPE_DIR : OVE_FS_TYPE_FILE;
	return OVE_OK;
}

int ove_fs_mkdir(const char *path)
{
	char fullpath[NATIVE_PATH_MAX];

	if (path == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
	}
	if (mkdir(fullpath, 0777) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_rmdir(const char *path)
{
	char fullpath[NATIVE_PATH_MAX];

	if (path == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
	}
	if (rmdir(fullpath) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_truncate(ove_file_t file, uint64_t length)
{
	if (file == NULL || length > (uint64_t)INT64_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (ftruncate(file->fd, (off_t)length) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}

int ove_fs_sync(ove_file_t file)
{
	if (file == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (fsync(file->fd) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}
