/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/fs.h"
#include "ove/media.h"
#include "ove/log.h"
#include "ove_backend_common.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/disk/sdmmc_stm32.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <string.h>
#include <stdio.h>
static FATFS fat_fs;
#if FF_MULTI_PARTITION
static char fatfs_mount_point[] = "/0:";
PARTITION VolToPart[FF_VOLUMES] = {
	{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4},
};
#else
static char fatfs_mount_point[] = "/SD:";
#endif
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = fatfs_mount_point,
};
static const uint8_t zero_fill[512] __aligned(32);
static int volume_mounted;
#define NATIVE_PATH_MAX (OVE_FS_PATH_MAX + 8)

/* Build full path with mount point prefix */
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
		needed = snprintf(buf, bufsz, "%s", mp.mnt_point);
	} else if (path[0] == '/') {
		needed = snprintf(buf, bufsz, "%s%s", mp.mnt_point, path);
	} else {
		needed = snprintf(buf, bufsz, "%s/%s", mp.mnt_point, path);
	}
	return needed < 0 || (size_t)needed >= bufsz ? OVE_ERR_NAME_TOO_LONG : OVE_OK;
}

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	(void)dev_path;
	if (mount_point != NULL && strcmp(mount_point, mp.mnt_point) != 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (volume_mounted) {
		return OVE_OK;
	}
	int lease_rc = ove_media_fs_acquire();
	if (lease_rc != OVE_OK)
		return lease_rc;

	int res = fs_mount(&mp);
	if (res != 0) {
		ove_media_fs_release();
		OVE_LOG_ERR("fs_mount failed: %d\n", res);
		return ove_errno_to_ove(-res);
	}
	volume_mounted = 1;
	OVE_LOG_DBG("SD card mounted at /SD:\n");
	return OVE_OK;
}

int ove_fs_mount_volume(const struct ove_fs_volume *volume, const char *mount_point)
{
	if (!volume || volume->logical_block_size != 512u || volume->block_count == 0u ||
	    volume->partition > 4u)
		return OVE_ERR_INVALID_PARAM;
#if FF_MULTI_PARTITION
	if (volume_mounted)
		return OVE_ERR_BUSY;
	fatfs_mount_point[1] = (char)('0' + volume->partition);
	return ove_fs_mount(NULL, mount_point);
#else
	if (volume->partition > 1u)
		return OVE_ERR_NOT_SUPPORTED;
	/* The single-volume configuration performs superfloppy/first-MBR discovery. */
	return ove_fs_mount(NULL, mount_point);
#endif
}

void ove_fs_unmount(const char *mount_point)
{
	if (!volume_mounted || (mount_point != NULL && strcmp(mount_point, mp.mnt_point) != 0)) {
		return;
	}
	if (fs_unmount(&mp) == 0) {
		volume_mounted = 0;
		ove_media_fs_release();
	}
}

int ove_fs_media_metrics(struct ove_fs_media_metrics *out_metrics)
{
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32_sdmmc)
	const struct device *dev =
		DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(st_stm32_sdmmc));
	struct stm32_sdmmc_metrics metrics;

	if (out_metrics == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (!device_is_ready(dev))
		return OVE_ERR_NOT_REGISTERED;
	stm32_sdmmc_get_metrics(dev, &metrics);
	out_metrics->read_commands = metrics.read_commands;
	out_metrics->write_commands = metrics.write_commands;
	out_metrics->read_blocks = metrics.read_blocks;
	out_metrics->write_blocks = metrics.write_blocks;
	out_metrics->multiblock_commands = metrics.multiblock_commands;
	out_metrics->completion_wait_us_total = metrics.completion_wait_us_total;
	out_metrics->completion_wait_us_max = metrics.completion_wait_us_max;
	out_metrics->ready_wait_us_total = metrics.ready_wait_us_total;
	out_metrics->ready_wait_us_max = metrics.ready_wait_us_max;
	out_metrics->errors = metrics.errors;
	out_metrics->recoveries = metrics.recoveries;
	return OVE_OK;
#else
	(void)out_metrics;
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

void ove_fs_media_metrics_reset(void)
{
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32_sdmmc)
	const struct device *dev =
		DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(st_stm32_sdmmc));

	if (device_is_ready(dev))
		stm32_sdmmc_reset_metrics(dev);
#endif
}

/* ─── _open_init / _close_deinit ─────────────────────────────────────── */

static int zflags_from(int flags)
{
	int zflags = 0;
	if (flags & OVE_FS_O_READ)
		zflags |= FS_O_READ;
	if (flags & OVE_FS_O_WRITE)
		zflags |= FS_O_WRITE;
	if (flags & OVE_FS_O_CREATE)
		zflags |= FS_O_CREATE;
	if (flags & OVE_FS_O_APPEND)
		zflags |= FS_O_APPEND;
	if (flags & OVE_FS_O_TRUNC)
		zflags |= FS_O_TRUNC;
	if (zflags == 0)
		zflags = FS_O_READ;
	return zflags;
}

int ove_fs_open_init(ove_file_t *file, ove_file_storage_t *storage, const char *path, int flags)
{
	char fullpath[NATIVE_PATH_MAX];
	struct fs_dirent entry;

	if (file == NULL || storage == NULL || path == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_file *f = (struct ove_file *)storage;
	fs_file_t_init(&f->file);
	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
	}

	if ((flags & (OVE_FS_O_CREATE | OVE_FS_O_EXCL)) == (OVE_FS_O_CREATE | OVE_FS_O_EXCL)) {
		struct fs_dirent entry;
		int stat_res = fs_stat(fullpath, &entry);
		if (stat_res == 0) {
			return OVE_ERR_ALREADY_EXISTS;
		}
		if (stat_res != -ENOENT) {
			return ove_errno_to_ove(-stat_res);
		}
	}

	int res = fs_open(&f->file, fullpath, zflags_from(flags));
	if (res != 0) {
		OVE_LOG_ERR("fs_open(%s) failed: %d\n", fullpath, res);
		return ove_errno_to_ove(-res);
	}
	f->position = 0;
	f->native_position = 0;
	f->native_position_valid = 1;
	f->append = (flags & OVE_FS_O_APPEND) != 0;
	if ((flags & OVE_FS_O_TRUNC) != 0) {
		f->size = 0;
	} else {
		res = fs_stat(fullpath, &entry);
		if (res != 0) {
			(void)fs_close(&f->file);
			return ove_errno_to_ove(-res);
		}
		f->size = entry.size;
	}

	*file = f;
	return OVE_OK;
}

/*
 * Preserve the API-visible cursor without eagerly moving FatFs' cursor.
 * LXP implements pread/pwrite by saving and restoring that visible offset;
 * deferring the native seek lets adjacent positioned I/O stay sequential.
 */
static int position_native_cursor(struct ove_file *file)
{
	if (file->native_position_valid && file->native_position == file->position) {
		return OVE_OK;
	}
	if (file->position > (uint64_t)INT32_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}
	int res = fs_seek(&file->file, (off_t)file->position, FS_SEEK_SET);
	if (res != 0) {
		return ove_errno_to_ove(-res);
	}
	file->native_position = file->position;
	file->native_position_valid = 1;
	return OVE_OK;
}

int ove_fs_close_deinit(ove_file_t file)
{
	if (file == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int res = fs_close(&file->file);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
}

/* Compatibility wrappers retained for legacy zero-heap callers. */

#ifndef OVE_HEAP_FS
#define FS_POOL_FILES 4
static struct ove_file file_pool[FS_POOL_FILES];
static int file_pool_used[FS_POOL_FILES];

int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	for (int i = 0; i < FS_POOL_FILES; i++) {
		if (!file_pool_used[i]) {
			file_pool_used[i] = 1;
			int ret = ove_fs_open_init(file, &file_pool[i], path, flags);
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
#endif /* !OVE_HEAP_FS */

int ove_fs_read(ove_file_t file, void *buf, size_t count, size_t *bytes_read)
{
	if (file == NULL || (buf == NULL && count != 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	int res = position_native_cursor(file);
	if (res != OVE_OK) {
		return res;
	}
	ssize_t br = fs_read(&file->file, buf, count);
	if (br < 0) {
		return ove_errno_to_ove((int)-br);
	}
	file->position += (size_t)br;
	file->native_position += (size_t)br;
	if (bytes_read != NULL) {
		*bytes_read = (size_t)br;
	}
	return OVE_OK;
}

int ove_fs_write(ove_file_t file, const void *buf, size_t count, size_t *bytes_written)
{
	if (file == NULL || (buf == NULL && count != 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (file->append) {
		file->position = file->size;
	}
	int res = position_native_cursor(file);
	if (res != OVE_OK) {
		return res;
	}
	ssize_t bw = fs_write(&file->file, buf, count);
	if (bw < 0) {
		return ove_errno_to_ove((int)-bw);
	}
	file->position += (size_t)bw;
	file->native_position += (size_t)bw;
	if (file->position > file->size) {
		file->size = file->position;
	}
	if (bytes_written != NULL) {
		*bytes_written = (size_t)bw;
	}
	return OVE_OK;
}

int ove_fs_size(ove_file_t file, size_t *out_size)
{
	if (file == NULL || out_size == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (file->size > SIZE_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}
	*out_size = (size_t)file->size;
	return OVE_OK;
}

/* ─── _opendir_init / _closedir_deinit ──────────────────────────────── */

int ove_fs_opendir_init(ove_dir_t *dir, ove_dir_storage_t *storage, const char *path)
{
	char fullpath[NATIVE_PATH_MAX];

	if (dir == NULL || storage == NULL || path == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_dir *d = (struct ove_dir *)storage;
	fs_dir_t_init(&d->dir);

	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
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
	int res = fs_closedir(&dir->dir);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
}

#ifndef OVE_HEAP_FS
#define FS_POOL_DIRS 4
static struct ove_dir dir_pool[FS_POOL_DIRS];
static int dir_pool_used[FS_POOL_DIRS];

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
#endif /* !OVE_HEAP_FS */

int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	struct fs_dirent de;

	if (dir == NULL || entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int res = fs_readdir(&dir->dir, &de);
	if (res != 0 || de.name[0] == '\0') {
		entry->name[0] = '\0';
		return (res == 0) ? OVE_ERR_EOF : ove_errno_to_ove(-res);
	}

	strncpy(entry->name, de.name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	entry->size = (unsigned int)de.size;
	entry->is_dir = (de.type == FS_DIR_ENTRY_DIR) ? 1 : 0;
	return OVE_OK;
}

#ifndef OVE_HEAP_FS
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
	return ret;
}
#endif /* !OVE_HEAP_FS */

int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	uint64_t base;

	if (file == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	switch (whence) {
	case OVE_FS_SEEK_SET:
		base = 0;
		break;
	case OVE_FS_SEEK_CUR:
		base = file->position;
		break;
	case OVE_FS_SEEK_END:
		base = file->size;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}

	uint64_t position;
	if (offset < 0) {
		uint64_t magnitude = (uint64_t)(-(offset + 1L)) + 1u;
		if (magnitude > base) {
			return OVE_ERR_INVALID_PARAM;
		}
		position = base - magnitude;
	} else {
		position = base + (uint64_t)offset;
		if (position < base || position > (uint64_t)INT32_MAX) {
			return OVE_ERR_INVALID_PARAM;
		}
	}
	file->position = position;
	return OVE_OK;
}

long ove_fs_tell(ove_file_t file)
{
	if (file == NULL) {
		return -1;
	}
	return (long)file->position;
}

int ove_fs_unlink(const char *path)
{
	char fullpath[NATIVE_PATH_MAX];
	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
	}
	int res = fs_unlink(fullpath);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
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
	int res = fs_rename(old_full, new_full);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
}

int ove_fs_stat(const char *path, struct ove_fs_stat *out_stat)
{
	char fullpath[NATIVE_PATH_MAX];
	struct fs_dirent entry;

	if (path == NULL || out_stat == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = build_path(fullpath, sizeof(fullpath), path);
	if (ret != OVE_OK) {
		return ret;
	}
	int res = fs_stat(fullpath, &entry);
	if (res != 0) {
		return ove_errno_to_ove(-res);
	}
	out_stat->size = (entry.type == FS_DIR_ENTRY_DIR) ? 0u : (uint64_t)entry.size;
	out_stat->mtime_sec = 0;
	out_stat->type = (entry.type == FS_DIR_ENTRY_DIR) ? OVE_FS_TYPE_DIR : OVE_FS_TYPE_FILE;
	return OVE_OK;
}

int ove_fs_statvfs(struct ove_fs_statvfs *out_stat)
{
	struct fs_statvfs native;

	if (out_stat == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (!volume_mounted)
		return OVE_ERR_NOT_REGISTERED;
	int res = fs_statvfs(mp.mnt_point, &native);
	if (res != 0)
		return ove_errno_to_ove(-res);

	memset(out_stat, 0, sizeof(*out_stat));
	out_stat->blocks = native.f_blocks;
	out_stat->blocks_free = native.f_bfree;
	out_stat->blocks_available = native.f_bfree;
	out_stat->block_size = native.f_bsize;
	out_stat->fragment_size = native.f_frsize;
#if FF_USE_LFN != 0
	out_stat->name_max = FF_MAX_LFN;
#else
	out_stat->name_max = 12u;
#endif
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
	int res = fs_mkdir(fullpath);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
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
	int res = fs_unlink(fullpath);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
}

int ove_fs_truncate(ove_file_t file, uint64_t length)
{
	if (file == NULL || length > (uint64_t)INT32_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (length < file->size) {
		int res = fs_truncate(&file->file, (off_t)length);
		if (res != 0) {
			file->native_position_valid = 0;
			return ove_errno_to_ove(-res);
		}
		file->size = length;
		file->native_position = length;
		file->native_position_valid = 1;
		return OVE_OK;
	}

	/*
	 * Zephyr's FatFs wrapper expands files with one-byte f_write() calls.
	 * Positioned SQLite writes can create large zero-filled gaps, so that
	 * implementation can monopolize the serialized host-FS worker for
	 * minutes. Grow explicitly in bounded chunks while preserving both POSIX
	 * hole contents and the API-visible cursor.
	 */
	if (length > file->size) {
		uint64_t logical_position = file->position;
		file->position = file->size;
		int res = position_native_cursor(file);
		if (res != OVE_OK) {
			file->position = logical_position;
			return res;
		}
		while (file->size < length) {
			size_t chunk = (size_t)(length - file->size);
			if (chunk > sizeof(zero_fill)) {
				chunk = sizeof(zero_fill);
			}
			ssize_t written = fs_write(&file->file, zero_fill, chunk);
			if (written <= 0) {
				file->position = logical_position;
				file->native_position_valid = 0;
				return written < 0 ? ove_errno_to_ove((int)-written) : OVE_ERR_IO;
			}
			file->size += (uint64_t)written;
			file->native_position += (uint64_t)written;
		}
		file->position = logical_position;
	}
	return OVE_OK;
}

int ove_fs_sync(ove_file_t file)
{
	if (file == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int res = fs_sync(&file->file);
	return (res == 0) ? OVE_OK : ove_errno_to_ove(-res);
}
