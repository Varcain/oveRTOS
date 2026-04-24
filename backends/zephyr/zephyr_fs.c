/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/fs.h"
#include "ove/log.h"
#include "ove_backend_common.h"
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <string.h>
#include <stdio.h>
static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};

/* Build full path with mount point prefix */
static void build_path(char *buf, size_t bufsz, const char *path)
{
	if (path[0] == '/') {
		/* Absolute path - use mount point + path */
		snprintf(buf, bufsz, "/SD:%s", path);
	} else {
		/* Relative path - prefix with mount root */
		snprintf(buf, bufsz, "/SD:/%s", path);
	}
}

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	(void)dev_path;
	(void)mount_point;

	int res = fs_mount(&mp);
	if (res != 0) {
		OVE_LOG_ERR("fs_mount failed: %d\n", res);
		return ove_errno_to_ove(-res);
	}
	OVE_LOG_DBG("SD card mounted at /SD:\n");
	return OVE_OK;
}

void ove_fs_unmount(const char *mount_point)
{
	(void)mount_point;
	fs_unmount(&mp);
}

/* ─── _open_init / _close_deinit ─────────────────────────────────────── */

static int zflags_from(int flags)
{
	int zflags = 0;
	if (flags & OVE_FS_O_READ)   zflags |= FS_O_READ;
	if (flags & OVE_FS_O_WRITE)  zflags |= FS_O_WRITE;
	if (flags & OVE_FS_O_CREATE) zflags |= FS_O_CREATE;
	if (flags & OVE_FS_O_APPEND) zflags |= FS_O_APPEND;
	if (zflags == 0) zflags = FS_O_READ;
	return zflags;
}

int ove_fs_open_init(ove_file_t *file, ove_file_storage_t *storage,
		     const char *path, int flags)
{
	char fullpath[128];

	if (file == NULL || storage == NULL || path == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_file *f = (struct ove_file *)storage;
	fs_file_t_init(&f->file);
	build_path(fullpath, sizeof(fullpath), path);

	int res = fs_open(&f->file, fullpath, zflags_from(flags));
	if (res != 0) {
		OVE_LOG_ERR("fs_open(%s) failed: %d\n", fullpath, res);
		return ove_errno_to_ove(-res);
	}

	*file = f;
	return OVE_OK;
}

int ove_fs_close_deinit(ove_file_t file)
{
	if (file == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	fs_close(&file->file);
	return OVE_OK;
}

/* ─── _open / _close — heap or zero-heap pool ─────────────────────── */

#ifdef OVE_HEAP_FS
int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	struct ove_file *f = OVE_BACKEND_MALLOC(sizeof(*f));
	if (f == NULL) {
		OVE_LOG_ERR("fs_open: k_malloc failed\n");
		return OVE_ERR_NO_MEMORY;
	}

	int ret = ove_fs_open_init(file, f, path, flags);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(f);
	}
	return ret;
}

int ove_fs_close(ove_file_t file)
{
	int ret = ove_fs_close_deinit(file);
	if (ret == OVE_OK) {
		OVE_BACKEND_FREE(file);
	}
	return ret;
}
#else /* zero-heap: static pool */
#define FS_POOL_FILES  4
static struct ove_file file_pool[FS_POOL_FILES];
static int             file_pool_used[FS_POOL_FILES];

int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	for (int i = 0; i < FS_POOL_FILES; i++) {
		if (!file_pool_used[i]) {
			file_pool_used[i] = 1;
			int ret = ove_fs_open_init(file, &file_pool[i],
						   path, flags);
			if (ret != OVE_OK) {
				file_pool_used[i] = 0;
			}
			return ret;
		}
	}
	return OVE_ERR_NO_MEMORY;
}

int ove_fs_close(ove_file_t file)
{
	int ret = ove_fs_close_deinit(file);
	for (int i = 0; i < FS_POOL_FILES; i++) {
		if (&file_pool[i] == file) {
			file_pool_used[i] = 0;
			break;
		}
	}
	return ret;
}
#endif /* OVE_HEAP_FS */

int ove_fs_read(ove_file_t file, void *buf, size_t count,
			  size_t *bytes_read)
{
	ssize_t br = fs_read(&file->file, buf, count);
	if (br < 0) {
		return ove_errno_to_ove((int)-br);
	}
	if (bytes_read != NULL) {
		*bytes_read = (size_t)br;
	}
	return OVE_OK;
}

int ove_fs_write(ove_file_t file, const void *buf,
			   size_t count, size_t *bytes_written)
{
	ssize_t bw = fs_write(&file->file, buf, count);
	if (bw < 0) {
		return ove_errno_to_ove((int)-bw);
	}
	if (bytes_written != NULL) {
		*bytes_written = (size_t)bw;
	}
	return OVE_OK;
}

int ove_fs_size(ove_file_t file, size_t *out_size)
{
	off_t cur = fs_tell(&file->file);
	if (cur < 0) {
		return ove_errno_to_ove((int)-cur);
	}
	int res = fs_seek(&file->file, 0, FS_SEEK_END);
	if (res != 0) {
		return ove_errno_to_ove(-res);
	}
	off_t end = fs_tell(&file->file);
	if (end < 0) {
		fs_seek(&file->file, cur, FS_SEEK_SET);
		return ove_errno_to_ove((int)-end);
	}
	fs_seek(&file->file, cur, FS_SEEK_SET);
	*out_size = (size_t)end;
	return OVE_OK;
}

/* ─── _opendir_init / _closedir_deinit ──────────────────────────────── */

int ove_fs_opendir_init(ove_dir_t *dir, ove_dir_storage_t *storage,
			const char *path)
{
	char fullpath[128];

	if (dir == NULL || storage == NULL || path == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_dir *d = (struct ove_dir *)storage;
	fs_dir_t_init(&d->dir);

	/* Map "/" to mount point root */
	if (path[0] == '/' && path[1] == '\0') {
		snprintf(fullpath, sizeof(fullpath), "/SD:");
	} else {
		build_path(fullpath, sizeof(fullpath), path);
	}

	int res = fs_opendir(&d->dir, fullpath);
	if (res != 0) {
		OVE_LOG_ERR("fs_opendir(%s) failed: %d\n", fullpath, res);
		return ove_errno_to_ove(-res);
	}

	*dir = d;
	return OVE_OK;
}

int ove_fs_closedir_deinit(ove_dir_t dir)
{
	if (dir == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	fs_closedir(&dir->dir);
	return OVE_OK;
}

/* ─── _opendir / _closedir — heap or zero-heap pool ─────────────────── */

#ifdef OVE_HEAP_FS
int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	struct ove_dir *d = OVE_BACKEND_MALLOC(sizeof(*d));
	if (d == NULL) {
		OVE_LOG_ERR("opendir: k_malloc failed\n");
		return OVE_ERR_NO_MEMORY;
	}

	int ret = ove_fs_opendir_init(dir, d, path);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(d);
	}
	return ret;
}
#else /* zero-heap: static pool */
#define FS_POOL_DIRS 4
static struct ove_dir dir_pool[FS_POOL_DIRS];
static int            dir_pool_used[FS_POOL_DIRS];

int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	for (int i = 0; i < FS_POOL_DIRS; i++) {
		if (!dir_pool_used[i]) {
			dir_pool_used[i] = 1;
			int ret = ove_fs_opendir_init(dir, &dir_pool[i], path);
			if (ret != OVE_OK) {
				dir_pool_used[i] = 0;
			}
			return ret;
		}
	}
	return OVE_ERR_NO_MEMORY;
}
#endif /* OVE_HEAP_FS */

int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	struct fs_dirent de;

	int res = fs_readdir(&dir->dir, &de);
	if (res != 0 || de.name[0] == '\0') {
		entry->name[0] = '\0';
		return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
	}

	strncpy(entry->name, de.name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	entry->size = (unsigned int)de.size;
	entry->is_dir = (de.type == FS_DIR_ENTRY_DIR) ? 1 : 0;
	return OVE_OK;
}

int ove_fs_closedir(ove_dir_t dir)
{
	int ret = ove_fs_closedir_deinit(dir);
#ifdef OVE_HEAP_FS
	if (ret == OVE_OK) {
		OVE_BACKEND_FREE(dir);
	}
#else
	for (int i = 0; i < FS_POOL_DIRS; i++) {
		if (&dir_pool[i] == dir) {
			dir_pool_used[i] = 0;
			break;
		}
	}
#endif
	return ret;
}

int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	int zwhence;

	switch (whence) {
	case OVE_FS_SEEK_SET: zwhence = FS_SEEK_SET; break;
	case OVE_FS_SEEK_CUR: zwhence = FS_SEEK_CUR; break;
	case OVE_FS_SEEK_END: zwhence = FS_SEEK_END; break;
	default: return OVE_ERR_INVALID_PARAM;
	}

	int res = fs_seek(&file->file, (off_t)offset, zwhence);
	if (res != 0) {
		return ove_errno_to_ove(-res);
	}
	return OVE_OK;
}

long ove_fs_tell(ove_file_t file)
{
	return (long)fs_tell(&file->file);
}

int ove_fs_unlink(const char *path)
{
	char fullpath[128];
	build_path(fullpath, sizeof(fullpath), path);
	int res = fs_unlink(fullpath);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	char old_full[128], new_full[128];
	build_path(old_full, sizeof(old_full), old_path);
	build_path(new_full, sizeof(new_full), new_path);
	int res = fs_rename(old_full, new_full);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
}
