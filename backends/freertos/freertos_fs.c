/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* Define the real FS structs before storage.h provides its stubs. */
#include "FreeRTOS.h"
#include "ff.h"
#include "ff_gen_drv.h"
#include "sd_diskio.h"

struct ove_file {
	FIL fil;
	FILINFO fno; /* cached info from open */
};

struct ove_dir {
	DIR dir;
};

#define OVE_FS_DEFINED

#include "ove/fs.h"
#include "ove/storage.h"
#include "ove/log.h"
#include "ove_backend_common.h"
#include <string.h>
/* Static path buffer for FatFS driver linking */
static char fatfs_path[16];
static FATFS fatfs;

int ove_fs_mount(const char *dev_path, const char *mount_point)
{
	FRESULT fres;
	(void)dev_path;
	(void)mount_point;

	FATFS_LinkDriver(&SD_Driver, fatfs_path);
	fres = f_mount(&fatfs, "", 0);
	if (fres != FR_OK) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

void ove_fs_unmount(const char *mount_point)
{
	(void)mount_point;
	f_mount(NULL, "", 0);
}

/* ─── _init / _deinit (static storage) ───────────────────────────────── */

int ove_fs_open_init(ove_file_t *file, ove_file_storage_t *storage, const char *path, int flags)
{
	struct ove_file *f = (struct ove_file *)storage;
	BYTE mode = 0;
	FRESULT fres;

	if (file == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (flags & OVE_FS_O_READ) {
		mode |= FA_READ;
	}
	if (flags & OVE_FS_O_WRITE) {
		mode |= FA_WRITE;
	}
	if (flags & OVE_FS_O_CREATE) {
		mode |= FA_CREATE_ALWAYS;
	}
	if (flags & OVE_FS_O_APPEND) {
		mode |= FA_OPEN_APPEND;
	}
	if (mode == 0) {
		mode = FA_READ;
	}

	fres = f_open(&f->fil, path, mode);
	if (fres != FR_OK) {
		OVE_LOG("fs: f_open(%s) failed: FRESULT=%d\n", path, (int)fres);
		return OVE_ERR_NOT_SUPPORTED;
	}

	*file = f;
	return OVE_OK;
}

int ove_fs_close_deinit(ove_file_t file)
{
	f_close(&file->fil);
	return OVE_OK;
}

int ove_fs_opendir_init(ove_dir_t *dir, ove_dir_storage_t *storage, const char *path)
{
	struct ove_dir *d = (struct ove_dir *)storage;
	FRESULT fres;

	if (dir == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	fres = f_opendir(&d->dir, path);
	if (fres != FR_OK) {
		OVE_LOG("fs: f_opendir(%s) failed: FRESULT=%d\n", path, (int)fres);
		return OVE_ERR_NOT_SUPPORTED;
	}

	*dir = d;
	return OVE_OK;
}

int ove_fs_closedir_deinit(ove_dir_t dir)
{
	f_closedir(&dir->dir);
	return OVE_OK;
}

/* ─── _create / _destroy (heap-gated) ────────────────────────────────── */

#ifdef OVE_HEAP_FS
int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	struct ove_file *f;
	BYTE mode = 0;
	FRESULT fres;

	if (flags & OVE_FS_O_READ) {
		mode |= FA_READ;
	}
	if (flags & OVE_FS_O_WRITE) {
		mode |= FA_WRITE;
	}
	if (flags & OVE_FS_O_CREATE) {
		mode |= FA_CREATE_ALWAYS;
	}
	if (flags & OVE_FS_O_APPEND) {
		mode |= FA_OPEN_APPEND;
	}
	if (mode == 0) {
		mode = FA_READ;
	}

	f = OVE_BACKEND_MALLOC(sizeof(*f));
	if (f == NULL) {
		OVE_LOG("fs: alloc failed (%u bytes)\n", (unsigned int)sizeof(*f));
		return OVE_ERR_NO_MEMORY;
	}

	fres = f_open(&f->fil, path, mode);
	if (fres != FR_OK) {
		OVE_LOG("fs: f_open(%s) failed: FRESULT=%d\n", path, (int)fres);
		OVE_BACKEND_FREE(f);
		return OVE_ERR_NOT_SUPPORTED;
	}

	*file = f;
	return OVE_OK;
}

int ove_fs_close(ove_file_t file)
{
	f_close(&file->fil);
	OVE_BACKEND_FREE(file);
	return OVE_OK;
}

int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	struct ove_dir *d;
	FRESULT fres;

	d = OVE_BACKEND_MALLOC(sizeof(*d));
	if (d == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	fres = f_opendir(&d->dir, path);
	if (fres != FR_OK) {
		OVE_LOG("fs: f_opendir(%s) failed: FRESULT=%d\n", path, (int)fres);
		OVE_BACKEND_FREE(d);
		return OVE_ERR_NOT_SUPPORTED;
	}

	*dir = d;
	return OVE_OK;
}

int ove_fs_closedir(ove_dir_t dir)
{
	f_closedir(&dir->dir);
	OVE_BACKEND_FREE(dir);
	return OVE_OK;
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
	BYTE mode = 0;
	FRESULT fres;

	if (flags & OVE_FS_O_READ)
		mode |= FA_READ;
	if (flags & OVE_FS_O_WRITE)
		mode |= FA_WRITE;
	if (flags & OVE_FS_O_CREATE)
		mode |= FA_CREATE_ALWAYS;
	if (flags & OVE_FS_O_APPEND)
		mode |= FA_OPEN_APPEND;
	if (mode == 0)
		mode = FA_READ;

	for (int i = 0; i < FS_POOL_FILES; i++) {
		if (!file_pool_used[i]) {
			file_pool_used[i] = 1;
			fres = f_open(&file_pool[i].fil, path, mode);
			if (fres != FR_OK) {
				file_pool_used[i] = 0;
				return OVE_ERR_NOT_SUPPORTED;
			}
			*file = &file_pool[i];
			return OVE_OK;
		}
	}
	return OVE_ERR_NO_MEMORY;
}

int ove_fs_close(ove_file_t file)
{
	f_close(&file->fil);
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

	for (int i = 0; i < FS_POOL_DIRS; i++) {
		if (!dir_pool_used[i]) {
			dir_pool_used[i] = 1;
			fres = f_opendir(&dir_pool[i].dir, path);
			if (fres != FR_OK) {
				dir_pool_used[i] = 0;
				return OVE_ERR_NOT_SUPPORTED;
			}
			*dir = &dir_pool[i];
			return OVE_OK;
		}
	}
	return OVE_ERR_NO_MEMORY;
}

int ove_fs_closedir(ove_dir_t dir)
{
	f_closedir(&dir->dir);
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

	fres = f_read(&file->fil, buf, count, &br);
	if (fres != FR_OK) {
		OVE_LOG("fs: f_read failed: FRESULT=%d (count=%u)\n", (int)fres,
			(unsigned int)count);
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (bytes_read != NULL) {
		*bytes_read = br;
	}
	return OVE_OK;
}

int ove_fs_write(ove_file_t file, const void *buf, size_t count, size_t *bytes_written)
{
	unsigned int bw;
	FRESULT fres;

	fres = f_write(&file->fil, buf, count, &bw);
	if (fres != FR_OK) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (bytes_written != NULL) {
		*bytes_written = bw;
	}
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
		return (fres == FR_OK) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
	}

	strncpy(entry->name, fno.fname, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	entry->size = (unsigned int)fno.fsize;
	entry->is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
	return OVE_OK;
}

int ove_fs_seek(ove_file_t file, long offset, int whence)
{
	DWORD pos;

	switch (whence) {
	case OVE_FS_SEEK_SET:
		pos = (DWORD)offset;
		break;
	case OVE_FS_SEEK_CUR:
		pos = f_tell(&file->fil) + (DWORD)offset;
		break;
	case OVE_FS_SEEK_END:
		pos = f_size(&file->fil) + (DWORD)offset;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}

	FRESULT fres = f_lseek(&file->fil, pos);
	if (fres != FR_OK) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

long ove_fs_tell(ove_file_t file)
{
	return (long)f_tell(&file->fil);
}

int ove_fs_unlink(const char *path)
{
	FRESULT fres = f_unlink(path);
	if (fres != FR_OK) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

int ove_fs_rename(const char *old_path, const char *new_path)
{
	FRESULT fres = f_rename(old_path, new_path);
	if (fres != FR_OK) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}
