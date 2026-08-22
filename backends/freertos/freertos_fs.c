/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "FreeRTOS.h"
#include "ff.h"
#include "ff_gen_drv.h"
#include "sd_diskio.h"

#include "ove/fs.h"
#include "ove/media.h"
#include "ove/storage.h"
#include "ove/log.h"
#include "ove_backend_common.h"
#include <stdio.h>
#include <string.h>
/* Static path buffer for FatFS driver linking */
static char fatfs_path[16];
static char active_volume[3] = "0:";
static FATFS fatfs;
static int driver_linked;
static const uint8_t zero_fill[512];

#if _MULTI_PARTITION
PARTITION VolToPart[_VOLUMES] = {
	{0, 0}, /* whole disk / automatic superfloppy-or-first-partition search */
	{0, 1}, {0, 2}, {0, 3}, {0, 4},
};
#endif

static FRESULT position_native_cursor(struct ove_file *file)
{
	if (f_tell(&file->fil) == file->position) {
		return FR_OK;
	}
	return f_lseek(&file->fil, file->position);
}

static FRESULT extend_zero_filled(struct ove_file *file, FSIZE_t length)
{
	FSIZE_t size = f_size(&file->fil);
	if (length <= size) {
		return FR_OK;
	}

	FRESULT result = f_lseek(&file->fil, size);
	while (result == FR_OK && size < length) {
		FSIZE_t remaining = length - size;
		UINT chunk = remaining < sizeof(zero_fill) ? (UINT)remaining : sizeof(zero_fill);
		UINT written = 0;
		result = f_write(&file->fil, zero_fill, chunk, &written);
		if (result == FR_OK && written != chunk) {
			result = FR_DISK_ERR;
		}
		size += written;
	}
	return result;
}

static int validate_path(const char *path)
{
	if (path == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	return strnlen(path, OVE_FS_PATH_MAX) < OVE_FS_PATH_MAX ? OVE_OK : OVE_ERR_NAME_TOO_LONG;
}

static int build_path(char *out, size_t capacity, const char *path)
{
	int rc = validate_path(path);
	if (rc != OVE_OK)
		return rc;
	int needed = path[0] == '/' ? snprintf(out, capacity, "%s%s", active_volume, path)
				    : snprintf(out, capacity, "%s/%s", active_volume, path);
	return needed < 0 || (size_t)needed >= capacity ? OVE_ERR_NAME_TOO_LONG : OVE_OK;
}

static int fatfs_result(FRESULT result)
{
	switch (result) {
	case FR_OK:
		return OVE_OK;
	case FR_NO_FILE:
	case FR_NO_PATH:
		return OVE_ERR_NOT_FOUND;
	case FR_INVALID_NAME:
		return OVE_ERR_NAME_TOO_LONG;
	case FR_EXIST:
		return OVE_ERR_ALREADY_EXISTS;
	case FR_WRITE_PROTECTED:
		return OVE_ERR_READ_ONLY;
	case FR_INVALID_OBJECT:
		return OVE_ERR_BAD_HANDLE;
	case FR_TIMEOUT:
		return OVE_ERR_TIMEOUT;
	case FR_LOCKED:
		return OVE_ERR_BUSY;
	case FR_NOT_ENOUGH_CORE:
		return OVE_ERR_NO_MEMORY;
	case FR_TOO_MANY_OPEN_FILES:
		return OVE_ERR_NO_MEMORY;
	case FR_DENIED:
		return OVE_ERR_PERMISSION;
	case FR_DISK_ERR:
	case FR_INT_ERR:
	case FR_NOT_READY:
		return OVE_ERR_IO;
	case FR_INVALID_DRIVE:
	case FR_INVALID_PARAMETER:
		return OVE_ERR_INVALID_PARAM;
	case FR_NOT_ENABLED:
	case FR_NO_FILESYSTEM:
	case FR_MKFS_ABORTED:
	default:
		return OVE_ERR_NOT_SUPPORTED;
	}
}

static int open_common(struct ove_file *file, const char *path, int flags)
{
	char native_path[OVE_FS_PATH_MAX + 4];
	BYTE mode = 0;

	int ret = build_path(native_path, sizeof(native_path), path);
	if (ret != OVE_OK) {
		return ret;
	}
	if (flags & OVE_FS_O_READ) {
		mode |= FA_READ;
	}
	if (flags & OVE_FS_O_WRITE) {
		mode |= FA_WRITE;
	}
	if (mode == 0) {
		mode = FA_READ;
	}
	if ((flags & (OVE_FS_O_CREATE | OVE_FS_O_EXCL)) == (OVE_FS_O_CREATE | OVE_FS_O_EXCL)) {
		mode |= FA_CREATE_NEW;
	} else if ((flags & (OVE_FS_O_CREATE | OVE_FS_O_TRUNC)) ==
		   (OVE_FS_O_CREATE | OVE_FS_O_TRUNC)) {
		mode |= FA_CREATE_ALWAYS;
	} else if (flags & OVE_FS_O_CREATE) {
		mode |= FA_OPEN_ALWAYS;
	}
	if ((flags & OVE_FS_O_TRUNC) && !(flags & OVE_FS_O_WRITE)) {
		return OVE_ERR_INVALID_PARAM;
	}

	FRESULT result = f_open(&file->fil, native_path, mode);
	if (result != FR_OK) {
		return fatfs_result(result);
	}
	if ((flags & OVE_FS_O_TRUNC) && !(flags & OVE_FS_O_CREATE)) {
		result = f_lseek(&file->fil, 0);
		if (result == FR_OK) {
			result = f_truncate(&file->fil);
		}
		if (result != FR_OK) {
			(void)f_close(&file->fil);
			return fatfs_result(result);
		}
	}
	file->append = (flags & OVE_FS_O_APPEND) != 0;
	file->position = 0;
	if (file->append) {
		file->position = f_size(&file->fil);
	}
	return OVE_OK;
}

static int mount_partition(uint8_t partition)
{
	FRESULT fres;
	if (partition >= _VOLUMES)
		return OVE_ERR_INVALID_PARAM;
	if (driver_linked)
		return active_volume[0] == (char)('0' + partition) ? OVE_OK : OVE_ERR_BUSY;
	int lease_rc = ove_media_fs_acquire();
	if (lease_rc != OVE_OK)
		return lease_rc;

	if (FATFS_LinkDriver(&SD_Driver, fatfs_path) != 0) {
		ove_media_fs_release();
		return OVE_ERR_NO_MEMORY;
	}
	driver_linked = 1;
	active_volume[0] = (char)('0' + partition);

	/* opt=1 probes the card and FAT volume now. A missing/unformatted card
	 * must fail mount rather than surfacing as an unrelated first-open error. */
	fres = f_mount(&fatfs, active_volume, 1);
	if (fres != FR_OK) {
		(void)FATFS_UnLinkDriver(fatfs_path);
		driver_linked = 0;
		ove_media_fs_release();
		return fatfs_result(fres);
	}
	return OVE_OK;
}

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	(void)dev_path;
	(void)mount_point;
	return mount_partition(0u);
}

int ove_fs_mount_volume(const struct ove_fs_volume *volume, const char *mount_point)
{
	if (!volume || volume->logical_block_size != 512u || volume->block_count == 0u ||
	    volume->partition > 4u)
		return OVE_ERR_INVALID_PARAM;
	(void)mount_point;
	return mount_partition(volume->partition);
}

void ove_fs_unmount(const char *mount_point)
{
	(void)mount_point;
	if (driver_linked) {
		(void)f_mount(NULL, active_volume, 0);
		(void)FATFS_UnLinkDriver(fatfs_path);
		driver_linked = 0;
		ove_media_fs_release();
	}
}

int ove_fs_media_metrics(struct ove_fs_media_metrics *out_metrics)
{
	(void)out_metrics;
	return OVE_ERR_NOT_SUPPORTED;
}

void ove_fs_media_metrics_reset(void)
{
}

/* ─── _init / _deinit (static storage) ───────────────────────────────── */

int ove_fs_open_init(ove_file_t *file, ove_file_storage_t *storage, const char *path, int flags)
{
	struct ove_file *f = (struct ove_file *)storage;
	if (file == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = open_common(f, path, flags);
	if (ret != OVE_OK) {
		OVE_LOG("fs: f_open(%s) failed: %d\n", path, ret);
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
	return fatfs_result(f_close(&file->fil));
}

int ove_fs_opendir_init(ove_dir_t *dir, ove_dir_storage_t *storage, const char *path)
{
	struct ove_dir *d = (struct ove_dir *)storage;
	FRESULT fres;
	char native_path[OVE_FS_PATH_MAX + 4];

	if (dir == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = build_path(native_path, sizeof(native_path), path);
	if (ret != OVE_OK) {
		return ret;
	}

	fres = f_opendir(&d->dir, native_path);
	if (fres != FR_OK) {
		OVE_LOG("fs: f_opendir(%s) failed: FRESULT=%d\n", path, (int)fres);
		return fatfs_result(fres);
	}

	*dir = d;
	return OVE_OK;
}

int ove_fs_closedir_deinit(ove_dir_t dir)
{
	if (dir == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	return fatfs_result(f_closedir(&dir->dir));
}

/* Compatibility wrappers retained for legacy zero-heap callers. */
#ifndef OVE_HEAP_FS
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

int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	FRESULT fres;
	char native_path[OVE_FS_PATH_MAX + 4];
	int ret = build_path(native_path, sizeof(native_path), path);
	if (ret != OVE_OK) {
		return ret;
	}

	for (int i = 0; i < FS_POOL_DIRS; i++) {
		if (!dir_pool_used[i]) {
			dir_pool_used[i] = 1;
			fres = f_opendir(&dir_pool[i].dir, native_path);
			if (fres != FR_OK) {
				dir_pool_used[i] = 0;
				return fatfs_result(fres);
			}
			*dir = &dir_pool[i];
			return OVE_OK;
		}
	}
	return OVE_ERR_NO_MEMORY;
}

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
#endif /* !OVE_HEAP_FS */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_fs_read(ove_file_t file, void *buf, size_t count, size_t *bytes_read)
{
	unsigned int br;
	FRESULT fres;

	if (file->position >= f_size(&file->fil)) {
		if (bytes_read != NULL) {
			*bytes_read = 0;
		}
		return OVE_OK;
	}
	fres = position_native_cursor(file);
	if (fres != FR_OK) {
		return fatfs_result(fres);
	}
	fres = f_read(&file->fil, buf, count, &br);
	if (fres != FR_OK) {
		OVE_LOG("fs: f_read failed: FRESULT=%d (count=%u)\n", (int)fres,
			(unsigned int)count);
		return fatfs_result(fres);
	}
	if (bytes_read != NULL) {
		*bytes_read = br;
	}
	file->position += br;
	return OVE_OK;
}

int ove_fs_write(ove_file_t file, const void *buf, size_t count, size_t *bytes_written)
{
	unsigned int bw;
	FRESULT fres;

	if (file->append) {
		file->position = f_size(&file->fil);
	}
	fres = extend_zero_filled(file, file->position);
	if (fres == FR_OK) {
		fres = position_native_cursor(file);
	}
	if (fres != FR_OK) {
		return fatfs_result(fres);
	}
	fres = f_write(&file->fil, buf, count, &bw);
	if (fres != FR_OK) {
		return fatfs_result(fres);
	}
	if (bytes_written != NULL) {
		*bytes_written = bw;
	}
	file->position += bw;
	return OVE_OK;
}

int ove_fs_size(ove_file_t file, size_t *out_size)
{
	*out_size = f_size(&file->fil);
	return OVE_OK;
}

int ove_fs_readdir(ove_dir_t dir, struct ove_dirent *entry)
{
	FILINFO fno;
	FRESULT fres;

	fres = f_readdir(&dir->dir, &fno);
	if (fres != FR_OK || fno.fname[0] == 0) {
		entry->name[0] = '\0';
		return (fres == FR_OK) ? OVE_ERR_EOF : fatfs_result(fres);
	}

	strncpy(entry->name, fno.fname, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	entry->size = (unsigned int)fno.fsize;
	entry->is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
	return OVE_OK;
}

int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	uint64_t base;

	switch (whence) {
	case OVE_FS_SEEK_SET:
		base = 0;
		break;
	case OVE_FS_SEEK_CUR:
		base = file->position;
		break;
	case OVE_FS_SEEK_END:
		base = f_size(&file->fil);
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}

	uint64_t pos;
	if (offset < 0) {
		uint64_t magnitude = (uint64_t)(-(offset + 1L)) + 1u;
		if (magnitude > base) {
			return OVE_ERR_INVALID_PARAM;
		}
		pos = base - magnitude;
	} else {
		pos = base + (uint64_t)offset;
		if (pos < base) {
			return OVE_ERR_INVALID_PARAM;
		}
	}
	if ((uint64_t)(FSIZE_t)pos != pos) {
		return OVE_ERR_INVALID_PARAM;
	}

	file->position = (FSIZE_t)pos;
	return OVE_OK;
}

long ove_fs_tell(ove_file_t file)
{
	return (long)file->position;
}

int ove_fs_unlink(const char *path)
{
	char native_path[OVE_FS_PATH_MAX + 4];
	int ret = build_path(native_path, sizeof(native_path), path);
	if (ret != OVE_OK) {
		return ret;
	}
	FRESULT fres = f_unlink(native_path);
	if (fres != FR_OK) {
		return fatfs_result(fres);
	}
	return OVE_OK;
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	char native_old[OVE_FS_PATH_MAX + 4];
	char native_new[OVE_FS_PATH_MAX + 4];
	int ret = build_path(native_old, sizeof(native_old), old_path);
	if (ret == OVE_OK) {
		ret = build_path(native_new, sizeof(native_new), new_path);
	}
	if (ret != OVE_OK) {
		return ret;
	}
	FRESULT fres = f_rename(native_old, native_new);
	if (fres != FR_OK) {
		return fatfs_result(fres);
	}
	return OVE_OK;
}

int ove_fs_stat(const char *path, struct ove_fs_stat *out_stat)
{
	FILINFO info;
	char native_path[OVE_FS_PATH_MAX + 4];

	if (path == NULL || out_stat == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = build_path(native_path, sizeof(native_path), path);
	if (ret != OVE_OK) {
		return ret;
	}
	/*
	 * FatFs rejects "/" in f_stat() even though f_opendir("/") is the
	 * mounted volume root.  The portable contract exposes that root as a
	 * directory, and this backend can answer it without probing a child.
	 */
	if (strcmp(path, "/") == 0) {
		if (!driver_linked) {
			return OVE_ERR_NOT_REGISTERED;
		}
		memset(out_stat, 0, sizeof(*out_stat));
		out_stat->type = OVE_FS_TYPE_DIR;
		return OVE_OK;
	}
	FRESULT result = f_stat(native_path, &info);
	if (result != FR_OK) {
		return fatfs_result(result);
	}
	out_stat->size = (info.fattrib & AM_DIR) ? 0u : (uint64_t)info.fsize;
	out_stat->mtime_sec = 0;
	out_stat->type = (info.fattrib & AM_DIR) ? OVE_FS_TYPE_DIR : OVE_FS_TYPE_FILE;
	return OVE_OK;
}

int ove_fs_statvfs(struct ove_fs_statvfs *out_stat)
{
	DWORD free_clusters;
	FATFS *volume;

	if (out_stat == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (!driver_linked)
		return OVE_ERR_NOT_REGISTERED;
	FRESULT result = f_getfree(active_volume, &free_clusters, &volume);
	if (result != FR_OK)
		return fatfs_result(result);

#if _MAX_SS != _MIN_SS
	uint32_t sector_size = volume->ssize;
#else
	uint32_t sector_size = _MIN_SS;
#endif
	memset(out_stat, 0, sizeof(*out_stat));
	out_stat->blocks = volume->n_fatent >= 2u ? (uint64_t)volume->n_fatent - 2u : 0u;
	out_stat->blocks_free = free_clusters;
	out_stat->blocks_available = free_clusters;
	out_stat->block_size = sector_size;
	out_stat->fragment_size = sector_size * volume->csize;
#if _USE_LFN != 0
	out_stat->name_max = _MAX_LFN;
#else
	out_stat->name_max = 12u;
#endif
	return OVE_OK;
}

int ove_fs_mkdir(const char *path)
{
	char native_path[OVE_FS_PATH_MAX + 4];
	int ret = build_path(native_path, sizeof(native_path), path);
	if (ret != OVE_OK) {
		return ret;
	}
	return fatfs_result(f_mkdir(native_path));
}

int ove_fs_rmdir(const char *path)
{
	char native_path[OVE_FS_PATH_MAX + 4];
	int ret = build_path(native_path, sizeof(native_path), path);
	if (ret != OVE_OK) {
		return ret;
	}
	return fatfs_result(f_unlink(native_path));
}

int ove_fs_truncate(ove_file_t file, uint64_t length)
{
	if (file == NULL || (uint64_t)(FSIZE_t)length != length) {
		return OVE_ERR_INVALID_PARAM;
	}
	FSIZE_t target = (FSIZE_t)length;
	FRESULT result;
	if (target > f_size(&file->fil)) {
		result = extend_zero_filled(file, target);
	} else {
		result = f_lseek(&file->fil, target);
	}
	if (result == FR_OK && target < f_size(&file->fil)) {
		result = f_truncate(&file->fil);
	}
	return fatfs_result(result);
}

int ove_fs_sync(ove_file_t file)
{
	if (file == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	return fatfs_result(f_sync(&file->fil));
}
