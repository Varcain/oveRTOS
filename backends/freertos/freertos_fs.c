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
#include "ove/storage.h"
#include "ove/log.h"
#include "ove_backend_common.h"
#include <string.h>
/* Static path buffer for FatFS driver linking */
static char fatfs_path[16];
static FATFS fatfs;
static int driver_linked;
static const uint8_t zero_fill[512];

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
	BYTE mode = 0;

	int ret = validate_path(path);
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

	FRESULT result = f_open(&file->fil, path, mode);
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

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	FRESULT fres;
	(void)dev_path;
	(void)mount_point;

	if (!driver_linked) {
		if (FATFS_LinkDriver(&SD_Driver, fatfs_path) != 0) {
			return OVE_ERR_NO_MEMORY;
		}
		driver_linked = 1;
	}

	/* opt=1 probes the card and FAT volume now. A missing/unformatted card
	 * must fail mount rather than surfacing as an unrelated first-open error. */
	fres = f_mount(&fatfs, fatfs_path, 1);
	if (fres != FR_OK) {
		return fatfs_result(fres);
	}
	return OVE_OK;
}

void ove_fs_unmount(const char *mount_point)
{
	(void)mount_point;
	if (driver_linked) {
		(void)f_mount(NULL, fatfs_path, 0);
		(void)FATFS_UnLinkDriver(fatfs_path);
		driver_linked = 0;
	}
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

	if (dir == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = validate_path(path);
	if (ret != OVE_OK) {
		return ret;
	}

	fres = f_opendir(&d->dir, path);
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

/* ─── _create / _destroy (heap-gated) ────────────────────────────────── */

#ifdef OVE_HEAP_FS
int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	struct ove_file *f;

	f = OVE_BACKEND_MALLOC(sizeof(*f));
	if (f == NULL) {
		OVE_LOG("fs: alloc failed (%u bytes)\n", (unsigned int)sizeof(*f));
		return OVE_ERR_NO_MEMORY;
	}

	int ret = open_common(f, path, flags);
	if (ret != OVE_OK) {
		OVE_LOG("fs: f_open(%s) failed: %d\n", path, ret);
		OVE_BACKEND_FREE(f);
		return ret;
	}

	*file = f;
	return OVE_OK;
}

int ove_fs_close(ove_file_t file)
{
	int ret = ove_fs_close_deinit(file);
	if (ret == OVE_OK) {
		OVE_BACKEND_FREE(file);
	}
	return ret;
}

int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	struct ove_dir *d;
	FRESULT fres;

	d = OVE_BACKEND_MALLOC(sizeof(*d));
	if (d == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = validate_path(path);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(d);
		return ret;
	}
	fres = f_opendir(&d->dir, path);
	if (fres != FR_OK) {
		OVE_LOG("fs: f_opendir(%s) failed: FRESULT=%d\n", path, (int)fres);
		OVE_BACKEND_FREE(d);
		return fatfs_result(fres);
	}

	*dir = d;
	return OVE_OK;
}

int ove_fs_closedir(ove_dir_t dir)
{
	int ret = ove_fs_closedir_deinit(dir);
	if (ret == OVE_OK) {
		OVE_BACKEND_FREE(dir);
	}
	return ret;
}
#else /* zero-heap: use static pool */
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
	int ret = validate_path(path);
	if (ret != OVE_OK) {
		return ret;
	}

	for (int i = 0; i < FS_POOL_DIRS; i++) {
		if (!dir_pool_used[i]) {
			dir_pool_used[i] = 1;
			fres = f_opendir(&dir_pool[i].dir, path);
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
#endif /* OVE_HEAP_FS */

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
	int ret = validate_path(path);
	if (ret != OVE_OK) {
		return ret;
	}
	FRESULT fres = f_unlink(path);
	if (fres != FR_OK) {
		return fatfs_result(fres);
	}
	return OVE_OK;
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	int ret = validate_path(old_path);
	if (ret == OVE_OK) {
		ret = validate_path(new_path);
	}
	if (ret != OVE_OK) {
		return ret;
	}
	FRESULT fres = f_rename(old_path, new_path);
	if (fres != FR_OK) {
		return fatfs_result(fres);
	}
	return OVE_OK;
}

int ove_fs_stat(const char *path, struct ove_fs_stat *out_stat)
{
	FILINFO info;

	if (path == NULL || out_stat == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = validate_path(path);
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
	FRESULT result = f_stat(path, &info);
	if (result != FR_OK) {
		return fatfs_result(result);
	}
	out_stat->size = (info.fattrib & AM_DIR) ? 0u : (uint64_t)info.fsize;
	out_stat->mtime_sec = 0;
	out_stat->type = (info.fattrib & AM_DIR) ? OVE_FS_TYPE_DIR : OVE_FS_TYPE_FILE;
	return OVE_OK;
}

int ove_fs_mkdir(const char *path)
{
	int ret = validate_path(path);
	if (ret != OVE_OK) {
		return ret;
	}
	return fatfs_result(f_mkdir(path));
}

int ove_fs_rmdir(const char *path)
{
	int ret = validate_path(path);
	if (ret != OVE_OK) {
		return ret;
	}
	return fatfs_result(f_unlink(path));
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
