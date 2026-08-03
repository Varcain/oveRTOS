/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serialized oveRTOS storage provider for LXP's persistent /data mount.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_FS)

#include "lxp/lxp_config.h"
#include "lxp/lxp_fs_ops.h"
#include "ove/fs.h"
#include "ove/queue.h"
#include "ove/sync.h"
#include "ove/thread.h"
#include "ove/time.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define LXP_OVE_FS_WORKER_STACK 4096u
#define LXP_OVE_FS_WORKER_PRIORITY OVE_PRIO_ABOVE_NORMAL
#define LXP_OVE_FS_IO_CHUNK 4096u

_Static_assert(OVE_PRIO_ABOVE_NORMAL < OVE_PRIO_HIGH,
	       "native FS worker must remain below high-priority RT work");

/*
 * LXP has at most LXP_NHOSTFS_OPEN co-resident external descriptors. A single
 * tagged union therefore covers every possible file/directory split without
 * provisioning two independent worst-case pools.
 */
struct lxp_fs_file {
	ove_file_storage_t storage;
	ove_file_t native;
};

struct lxp_fs_dir {
	ove_dir_storage_t storage;
	ove_dir_t native;
};

enum fs_handle_kind {
	FS_HANDLE_FREE = 0,
	FS_HANDLE_FILE,
	FS_HANDLE_DIR,
};

struct fs_handle_slot {
	union {
		struct lxp_fs_file file;
		struct lxp_fs_dir dir;
	} handle;
	uint8_t kind;
};

enum fs_request_op {
	FS_REQ_MOUNT = 0,
	FS_REQ_FILE_OPEN,
	FS_REQ_OBJECT_OPEN,
	FS_REQ_FILE_CLOSE,
	FS_REQ_FILE_READ,
	FS_REQ_FILE_WRITE,
	FS_REQ_FILE_SEEK,
	FS_REQ_FILE_STAT,
	FS_REQ_FILE_TRUNCATE,
	FS_REQ_FILE_SYNC,
	FS_REQ_FILE_PREAD,
	FS_REQ_FILE_PWRITE,
	FS_REQ_DIR_OPEN,
	FS_REQ_DIR_READ,
	FS_REQ_DIR_CLOSE,
	FS_REQ_PATH_STAT,
	FS_REQ_PATH_MKDIR,
	FS_REQ_PATH_RMDIR,
	FS_REQ_PATH_UNLINK,
	FS_REQ_PATH_RENAME,
	FS_REQ_STOP,
};

struct fs_request {
	enum fs_request_op op;
	int result;
	uint8_t asynchronous;
	uint8_t budget_overrun;
	uint64_t submitted_us;
	uint64_t started_us;
	uint64_t finished_us;
	union {
		struct {
			const char *path;
			unsigned int flags;
			lxp_fs_file_t *out;
		} file_open;
		struct {
			const char *path;
			unsigned int flags;
			int require_dir;
			lxp_fs_open_result_t *out;
		} object_open;
		struct {
			lxp_fs_file_t file;
		} file;
		struct {
			lxp_fs_file_t file;
			void *buf;
			size_t count;
			size_t transferred;
		} file_read;
		struct {
			lxp_fs_file_t file;
			const void *buf;
			size_t count;
			size_t transferred;
		} file_write;
		struct {
			lxp_fs_file_t file;
			void *buf;
			size_t count;
			uint64_t offset;
			size_t transferred;
		} file_pread;
		struct {
			lxp_fs_file_t file;
			const void *buf;
			size_t count;
			uint64_t offset;
			size_t transferred;
		} file_pwrite;
		struct {
			lxp_fs_file_t file;
			int64_t offset;
			int whence;
			uint64_t *new_offset;
		} file_seek;
		struct {
			lxp_fs_file_t file;
			lxp_fs_stat_t *out;
		} file_stat;
		struct {
			lxp_fs_file_t file;
			uint64_t length;
		} file_truncate;
		struct {
			const char *path;
			lxp_fs_dir_t *out;
		} dir_open;
		struct {
			lxp_fs_dir_t dir;
			lxp_fs_dirent_t *entry;
		} dir_read;
		struct {
			lxp_fs_dir_t dir;
		} dir;
		struct {
			const char *path;
			lxp_fs_stat_t *out;
		} path_stat;
		struct {
			const char *path;
		} path;
		struct {
			const char *old_path;
			const char *new_path;
		} path_rename;
	} args;
};

static struct fs_handle_slot g_handles[LXP_NHOSTFS_OPEN];
static ove_mutex_storage_t g_submit_lock_storage;
static ove_mutex_t g_submit_lock;
static ove_event_storage_t g_complete_storage;
static ove_event_t g_complete;
static ove_queue_storage_t g_request_queue_storage;
static ove_queue_t g_request_queue;
static struct fs_request *g_request_queue_buffer[1];
static ove_thread_storage_t g_worker_storage;
static ove_thread_t g_worker;
OVE_THREAD_STACK_DEFINE_STATIC_(g_worker_stack, LXP_OVE_FS_WORKER_STACK);
/*
 * Native filesystems may hand their caller buffer directly to a block-device
 * DMA engine. Linux guest memory is cacheable external memory whose MPU and
 * lifetime are owned by the guest, so it must never cross either the native
 * worker-task or device boundary. The coordinator copies guest data while
 * holding g_submit_lock; the serialized worker only accesses this aligned
 * staging buffer.
 */
static uint8_t g_io_buffer[LXP_OVE_FS_IO_CHUNK] __attribute__((aligned(32)));
static int g_active;
static int g_mounted;
static lxp_fs_metrics_t g_metrics;
enum fs_async_state {
	FS_ASYNC_IDLE = 0,
	FS_ASYNC_ACTIVE,
	FS_ASYNC_COMPLETE,
};
static struct fs_request g_async_request;
static char g_async_path[LXP_FS_NAME_MAX];
static char g_async_new_path[LXP_FS_NAME_MAX];
static lxp_fs_file_t g_async_file;
static lxp_fs_dir_t g_async_dir;
static lxp_fs_open_result_t g_async_open;
static lxp_fs_stat_t g_async_stat;
static lxp_fs_dirent_t g_async_dirent;
static uint64_t g_async_offset;
static uint64_t g_selected_owner;
static uint64_t g_async_owner;
static int g_async_cancelled;
static int g_async_state;
static uint64_t g_server_period_start_us;
static unsigned int g_server_requests;

_Static_assert(LXP_FS_O_READ == OVE_FS_O_READ, "filesystem read flag drifted");
_Static_assert(LXP_FS_O_WRITE == OVE_FS_O_WRITE, "filesystem write flag drifted");
_Static_assert(LXP_FS_O_CREATE == OVE_FS_O_CREATE, "filesystem create flag drifted");
_Static_assert(LXP_FS_O_APPEND == OVE_FS_O_APPEND, "filesystem append flag drifted");
_Static_assert(LXP_FS_O_TRUNC == OVE_FS_O_TRUNC, "filesystem truncate flag drifted");
_Static_assert(LXP_FS_O_EXCL == OVE_FS_O_EXCL, "filesystem exclusive flag drifted");
_Static_assert(LXP_FS_SEEK_SET == OVE_FS_SEEK_SET, "filesystem seek-set drifted");
_Static_assert(LXP_FS_SEEK_CUR == OVE_FS_SEEK_CUR, "filesystem seek-cur drifted");
_Static_assert(LXP_FS_SEEK_END == OVE_FS_SEEK_END, "filesystem seek-end drifted");
_Static_assert(LXP_FS_TYPE_FILE == OVE_FS_TYPE_FILE, "filesystem file type drifted");
_Static_assert(LXP_FS_TYPE_DIR == OVE_FS_TYPE_DIR, "filesystem directory type drifted");

static struct fs_handle_slot *file_slot(lxp_fs_file_t file)
{
	if (file == NULL)
		return NULL;
	for (size_t i = 0; i < LXP_NHOSTFS_OPEN; i++)
		if (g_handles[i].kind == FS_HANDLE_FILE && &g_handles[i].handle.file == file)
			return &g_handles[i];
	return NULL;
}

static struct fs_handle_slot *dir_slot(lxp_fs_dir_t dir)
{
	if (dir == NULL)
		return NULL;
	for (size_t i = 0; i < LXP_NHOSTFS_OPEN; i++)
		if (g_handles[i].kind == FS_HANDLE_DIR && &g_handles[i].handle.dir == dir)
			return &g_handles[i];
	return NULL;
}

static struct fs_handle_slot *free_slot(void)
{
	for (size_t i = 0; i < LXP_NHOSTFS_OPEN; i++)
		if (g_handles[i].kind == FS_HANDLE_FREE)
			return &g_handles[i];
	return NULL;
}

static int ensure_mounted(void)
{
	if (g_mounted)
		return LXP_OK;
	if (ove_fs_mount(NULL, NULL) != OVE_OK)
		return LXP_ERR_NOT_REGISTERED;
	g_mounted = 1;
	return LXP_OK;
}

static void stat_to_lxp(lxp_fs_stat_t *out, const struct ove_fs_stat *native)
{
	memset(out, 0, sizeof(*out));
	out->size = native->size;
	out->mtime_sec = native->mtime_sec;
	out->type = native->type == OVE_FS_TYPE_DIR ? LXP_FS_TYPE_DIR : LXP_FS_TYPE_FILE;
}

static void close_all_handles(void)
{
	for (size_t i = 0; i < LXP_NHOSTFS_OPEN; i++) {
		if (g_handles[i].kind == FS_HANDLE_FILE)
			(void)ove_fs_close_deinit(g_handles[i].handle.file.native);
		else if (g_handles[i].kind == FS_HANDLE_DIR)
			(void)ove_fs_closedir_deinit(g_handles[i].handle.dir.native);
		memset(&g_handles[i], 0, sizeof(g_handles[i]));
	}
}

static int execute_request(struct fs_request *request)
{
	struct fs_handle_slot *slot;
	struct ove_fs_stat native_stat;
	struct ove_dirent native_entry;
	size_t size;
	long position;
	int rc;

	if (request->op == FS_REQ_MOUNT)
		return ensure_mounted();
	if (request->op == FS_REQ_STOP) {
		close_all_handles();
		if (g_mounted)
			ove_fs_unmount(NULL);
		g_mounted = 0;
		return LXP_OK;
	}
	rc = ensure_mounted();
	if (rc != LXP_OK)
		return rc;

	switch (request->op) {
	case FS_REQ_FILE_OPEN:
		if (request->args.file_open.path == NULL || request->args.file_open.out == NULL)
			return LXP_ERR_INVALID_PARAM;
		if ((request->args.file_open.flags &
		     ~(LXP_FS_O_READ | LXP_FS_O_WRITE | LXP_FS_O_CREATE | LXP_FS_O_APPEND |
		       LXP_FS_O_TRUNC | LXP_FS_O_EXCL)) != 0u ||
		    (request->args.file_open.flags & (LXP_FS_O_READ | LXP_FS_O_WRITE)) == 0u)
			return LXP_ERR_INVALID_PARAM;
		slot = free_slot();
		if (slot == NULL)
			return LXP_ERR_NO_MEMORY;
		memset(slot, 0, sizeof(*slot));
		rc = ove_fs_open_init(&slot->handle.file.native, &slot->handle.file.storage,
				      request->args.file_open.path,
				      (int)request->args.file_open.flags);
		if (rc != OVE_OK) {
			memset(slot, 0, sizeof(*slot));
			return rc;
		}
		slot->kind = FS_HANDLE_FILE;
		*request->args.file_open.out = &slot->handle.file;
		return LXP_OK;
	case FS_REQ_OBJECT_OPEN:
		if (request->args.object_open.path == NULL || request->args.object_open.out == NULL)
			return LXP_ERR_INVALID_PARAM;
		memset(request->args.object_open.out, 0, sizeof(*request->args.object_open.out));
		rc = ove_fs_stat(request->args.object_open.path, &native_stat);
		if (rc == OVE_OK && native_stat.type == OVE_FS_TYPE_DIR) {
			if ((request->args.object_open.flags & OVE_FS_O_WRITE) != 0u ||
			    (request->args.object_open.flags &
			     (OVE_FS_O_CREATE | OVE_FS_O_TRUNC)) != 0u)
				return LXP_ERR_IS_DIR;
			slot = free_slot();
			if (slot == NULL)
				return LXP_ERR_NO_MEMORY;
			memset(slot, 0, sizeof(*slot));
			rc = ove_fs_opendir_init(&slot->handle.dir.native, &slot->handle.dir.storage,
						 request->args.object_open.path);
			if (rc != OVE_OK) {
				memset(slot, 0, sizeof(*slot));
				return rc;
			}
			slot->kind = FS_HANDLE_DIR;
			request->args.object_open.out->handle.dir = &slot->handle.dir;
			request->args.object_open.out->type = LXP_FS_TYPE_DIR;
			return LXP_OK;
		}
		if (rc != OVE_OK && rc != OVE_ERR_NOT_FOUND)
			return rc;
		if (request->args.object_open.require_dir)
			return rc == OVE_ERR_NOT_FOUND ? rc : LXP_ERR_NOT_DIR;
		if ((request->args.object_open.flags &
		     ~(LXP_FS_O_READ | LXP_FS_O_WRITE | LXP_FS_O_CREATE | LXP_FS_O_APPEND |
		       LXP_FS_O_TRUNC | LXP_FS_O_EXCL)) != 0u ||
		    (request->args.object_open.flags & (LXP_FS_O_READ | LXP_FS_O_WRITE)) == 0u)
			return LXP_ERR_INVALID_PARAM;
		slot = free_slot();
		if (slot == NULL)
			return LXP_ERR_NO_MEMORY;
		memset(slot, 0, sizeof(*slot));
		rc = ove_fs_open_init(&slot->handle.file.native, &slot->handle.file.storage,
				      request->args.object_open.path,
				      (int)request->args.object_open.flags);
		if (rc != OVE_OK) {
			memset(slot, 0, sizeof(*slot));
			return rc;
		}
		slot->kind = FS_HANDLE_FILE;
		request->args.object_open.out->handle.file = &slot->handle.file;
		request->args.object_open.out->type = LXP_FS_TYPE_FILE;
		return LXP_OK;
	case FS_REQ_FILE_CLOSE:
		slot = file_slot(request->args.file.file);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		rc = ove_fs_close_deinit(slot->handle.file.native);
		if (rc == OVE_OK)
			memset(slot, 0, sizeof(*slot));
		return rc;
	case FS_REQ_FILE_READ:
		slot = file_slot(request->args.file_read.file);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		if (request->args.file_read.count > sizeof(g_io_buffer))
			return LXP_ERR_INVALID_PARAM;
		request->args.file_read.transferred = 0;
		return ove_fs_read(slot->handle.file.native, g_io_buffer,
				   request->args.file_read.count,
				   &request->args.file_read.transferred);
	case FS_REQ_FILE_WRITE:
		slot = file_slot(request->args.file_write.file);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		if (request->args.file_write.count > sizeof(g_io_buffer))
			return LXP_ERR_INVALID_PARAM;
		request->args.file_write.transferred = 0;
		return ove_fs_write(slot->handle.file.native, g_io_buffer,
				    request->args.file_write.count,
				    &request->args.file_write.transferred);
	case FS_REQ_FILE_PREAD:
	case FS_REQ_FILE_PWRITE: {
		int write = request->op == FS_REQ_FILE_PWRITE;
		lxp_fs_file_t file = write ? request->args.file_pwrite.file
					       : request->args.file_pread.file;
		size_t count = write ? request->args.file_pwrite.count
				     : request->args.file_pread.count;
		uint64_t offset = write ? request->args.file_pwrite.offset
					: request->args.file_pread.offset;
		slot = file_slot(file);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		if (count > sizeof(g_io_buffer) || offset > (uint64_t)LONG_MAX)
			return LXP_ERR_INVALID_PARAM;
		long saved = ove_fs_tell(slot->handle.file.native);
		if (saved < 0)
			return LXP_ERR_IO;
		rc = ove_fs_size(slot->handle.file.native, &size);
		if (rc != OVE_OK)
			return rc;
		if (!write && offset >= size) {
			request->args.file_pread.transferred = 0;
			return LXP_ERR_EOF;
		}
		if (write && offset > size) {
			rc = ove_fs_truncate(slot->handle.file.native, offset);
			if (rc != OVE_OK)
				return rc;
		}
		rc = ove_fs_seek(slot->handle.file.native, (long)offset, OVE_FS_SEEK_SET);
		if (rc != OVE_OK)
			return rc;
		if (write) {
			request->args.file_pwrite.transferred = 0;
			rc = ove_fs_write(slot->handle.file.native, g_io_buffer, count,
					  &request->args.file_pwrite.transferred);
		} else {
			request->args.file_pread.transferred = 0;
			rc = ove_fs_read(slot->handle.file.native, g_io_buffer, count,
					 &request->args.file_pread.transferred);
		}
		int restore = ove_fs_seek(slot->handle.file.native, saved, OVE_FS_SEEK_SET);
		return rc == OVE_OK || rc == OVE_ERR_EOF ? (restore == OVE_OK ? rc : restore) : rc;
	}
	case FS_REQ_FILE_SEEK:
		slot = file_slot(request->args.file_seek.file);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		if (request->args.file_seek.new_offset == NULL ||
		    request->args.file_seek.offset < INT32_MIN ||
		    request->args.file_seek.offset > INT32_MAX ||
		    request->args.file_seek.whence < OVE_FS_SEEK_SET ||
		    request->args.file_seek.whence > OVE_FS_SEEK_END)
			return LXP_ERR_INVALID_PARAM;
		rc = ove_fs_seek(slot->handle.file.native, (long)request->args.file_seek.offset,
				 request->args.file_seek.whence);
		if (rc != OVE_OK)
			return rc;
		position = ove_fs_tell(slot->handle.file.native);
		if (position < 0)
			return LXP_ERR_IO;
		*request->args.file_seek.new_offset = (uint64_t)(unsigned long)position;
		return LXP_OK;
	case FS_REQ_FILE_STAT:
		slot = file_slot(request->args.file_stat.file);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		if (request->args.file_stat.out == NULL)
			return LXP_ERR_INVALID_PARAM;
		rc = ove_fs_size(slot->handle.file.native, &size);
		if (rc != OVE_OK)
			return rc;
		memset(request->args.file_stat.out, 0, sizeof(*request->args.file_stat.out));
		request->args.file_stat.out->size = size;
		request->args.file_stat.out->type = LXP_FS_TYPE_FILE;
		return LXP_OK;
	case FS_REQ_FILE_TRUNCATE:
		slot = file_slot(request->args.file_truncate.file);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		return ove_fs_truncate(slot->handle.file.native,
				       request->args.file_truncate.length);
	case FS_REQ_FILE_SYNC:
		slot = file_slot(request->args.file.file);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		return ove_fs_sync(slot->handle.file.native);
	case FS_REQ_DIR_OPEN:
		if (request->args.dir_open.path == NULL || request->args.dir_open.out == NULL)
			return LXP_ERR_INVALID_PARAM;
		slot = free_slot();
		if (slot == NULL)
			return LXP_ERR_NO_MEMORY;
		memset(slot, 0, sizeof(*slot));
		rc = ove_fs_opendir_init(&slot->handle.dir.native, &slot->handle.dir.storage,
					 request->args.dir_open.path);
		if (rc != OVE_OK) {
			memset(slot, 0, sizeof(*slot));
			return rc;
		}
		slot->kind = FS_HANDLE_DIR;
		*request->args.dir_open.out = &slot->handle.dir;
		return LXP_OK;
	case FS_REQ_DIR_READ:
		slot = dir_slot(request->args.dir_read.dir);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		if (request->args.dir_read.entry == NULL)
			return LXP_ERR_INVALID_PARAM;
		rc = ove_fs_readdir(slot->handle.dir.native, &native_entry);
		if (rc != OVE_OK)
			return rc;
		memset(request->args.dir_read.entry, 0, sizeof(*request->args.dir_read.entry));
		memcpy(request->args.dir_read.entry->name, native_entry.name,
		       sizeof(request->args.dir_read.entry->name));
		request->args.dir_read.entry->name[LXP_FS_NAME_MAX - 1u] = '\0';
		request->args.dir_read.entry->size = native_entry.size;
		request->args.dir_read.entry->type = native_entry.is_dir ? LXP_FS_TYPE_DIR
									 : LXP_FS_TYPE_FILE;
		return LXP_OK;
	case FS_REQ_DIR_CLOSE:
		slot = dir_slot(request->args.dir.dir);
		if (slot == NULL)
			return LXP_ERR_BAD_HANDLE;
		rc = ove_fs_closedir_deinit(slot->handle.dir.native);
		if (rc == OVE_OK)
			memset(slot, 0, sizeof(*slot));
		return rc;
	case FS_REQ_PATH_STAT:
		if (request->args.path_stat.path == NULL || request->args.path_stat.out == NULL)
			return LXP_ERR_INVALID_PARAM;
		rc = ove_fs_stat(request->args.path_stat.path, &native_stat);
		if (rc == OVE_OK)
			stat_to_lxp(request->args.path_stat.out, &native_stat);
		return rc;
	case FS_REQ_PATH_MKDIR:
		return request->args.path.path == NULL ? LXP_ERR_INVALID_PARAM
						       : ove_fs_mkdir(request->args.path.path);
	case FS_REQ_PATH_RMDIR:
		return request->args.path.path == NULL ? LXP_ERR_INVALID_PARAM
						       : ove_fs_rmdir(request->args.path.path);
	case FS_REQ_PATH_UNLINK:
		return request->args.path.path == NULL ? LXP_ERR_INVALID_PARAM
						       : ove_fs_unlink(request->args.path.path);
	case FS_REQ_PATH_RENAME:
		return request->args.path_rename.old_path == NULL ||
				       request->args.path_rename.new_path == NULL
			       ? LXP_ERR_INVALID_PARAM
			       : ove_fs_rename(request->args.path_rename.old_path,
					       request->args.path_rename.new_path);
	case FS_REQ_MOUNT:
	case FS_REQ_STOP:
	default:
		return LXP_ERR_INVALID_PARAM;
	}
}

static int async_copy_path(char dst[LXP_FS_NAME_MAX], const char *src)
{
	if (src == NULL)
		return LXP_ERR_INVALID_PARAM;
	size_t length = 0;
	while (length < LXP_FS_NAME_MAX && src[length] != '\0')
		length++;
	if (length == LXP_FS_NAME_MAX)
		return LXP_ERR_NAME_TOO_LONG;
	memcpy(dst, src, length + 1u);
	return LXP_OK;
}

static int async_prepare(const struct fs_request *source)
{
	g_async_request = *source;
	g_async_request.asynchronous = 1u;
	switch (source->op) {
	case FS_REQ_FILE_OPEN:
		if (async_copy_path(g_async_path, source->args.file_open.path) != LXP_OK)
			return LXP_ERR_NAME_TOO_LONG;
		g_async_request.args.file_open.path = g_async_path;
		g_async_request.args.file_open.out = &g_async_file;
		break;
	case FS_REQ_OBJECT_OPEN:
		if (async_copy_path(g_async_path, source->args.object_open.path) != LXP_OK)
			return LXP_ERR_NAME_TOO_LONG;
		g_async_request.args.object_open.path = g_async_path;
		g_async_request.args.object_open.out = &g_async_open;
		break;
	case FS_REQ_FILE_SEEK:
		g_async_request.args.file_seek.new_offset = &g_async_offset;
		break;
	case FS_REQ_FILE_STAT:
		g_async_request.args.file_stat.out = &g_async_stat;
		break;
	case FS_REQ_DIR_OPEN:
		if (async_copy_path(g_async_path, source->args.dir_open.path) != LXP_OK)
			return LXP_ERR_NAME_TOO_LONG;
		g_async_request.args.dir_open.path = g_async_path;
		g_async_request.args.dir_open.out = &g_async_dir;
		break;
	case FS_REQ_DIR_READ:
		g_async_request.args.dir_read.entry = &g_async_dirent;
		break;
	case FS_REQ_PATH_STAT:
		if (async_copy_path(g_async_path, source->args.path_stat.path) != LXP_OK)
			return LXP_ERR_NAME_TOO_LONG;
		g_async_request.args.path_stat.path = g_async_path;
		g_async_request.args.path_stat.out = &g_async_stat;
		break;
	case FS_REQ_PATH_MKDIR:
	case FS_REQ_PATH_RMDIR:
	case FS_REQ_PATH_UNLINK:
		if (async_copy_path(g_async_path, source->args.path.path) != LXP_OK)
			return LXP_ERR_NAME_TOO_LONG;
		g_async_request.args.path.path = g_async_path;
		break;
	case FS_REQ_PATH_RENAME:
		if (async_copy_path(g_async_path, source->args.path_rename.old_path) != LXP_OK ||
		    async_copy_path(g_async_new_path, source->args.path_rename.new_path) != LXP_OK)
			return LXP_ERR_NAME_TOO_LONG;
		g_async_request.args.path_rename.old_path = g_async_path;
		g_async_request.args.path_rename.new_path = g_async_new_path;
		break;
	default:
		break;
	}
	return LXP_OK;
}

static int async_collect(struct fs_request *request)
{
	if (request->op != g_async_request.op)
		return LXP_ERR_INVALID_PARAM;
	switch (request->op) {
	case FS_REQ_FILE_OPEN:
		if (request->args.file_open.out)
			*request->args.file_open.out = g_async_file;
		break;
	case FS_REQ_OBJECT_OPEN:
		if (request->args.object_open.out)
			*request->args.object_open.out = g_async_open;
		break;
	case FS_REQ_FILE_READ:
		request->args.file_read.transferred = g_async_request.args.file_read.transferred;
		if (request->args.file_read.transferred > request->args.file_read.count)
			return LXP_ERR_IO;
		if (request->args.file_read.transferred != 0u)
			memcpy(request->args.file_read.buf, g_io_buffer,
			       request->args.file_read.transferred);
		break;
	case FS_REQ_FILE_WRITE:
		request->args.file_write.transferred = g_async_request.args.file_write.transferred;
		break;
	case FS_REQ_FILE_PREAD:
		request->args.file_pread.transferred = g_async_request.args.file_pread.transferred;
		if (request->args.file_pread.transferred > request->args.file_pread.count)
			return LXP_ERR_IO;
		if (request->args.file_pread.transferred != 0u)
			memcpy(request->args.file_pread.buf, g_io_buffer,
			       request->args.file_pread.transferred);
		break;
	case FS_REQ_FILE_PWRITE:
		request->args.file_pwrite.transferred = g_async_request.args.file_pwrite.transferred;
		break;
	case FS_REQ_FILE_SEEK:
		if (request->args.file_seek.new_offset)
			*request->args.file_seek.new_offset = g_async_offset;
		break;
	case FS_REQ_FILE_STAT:
		if (request->args.file_stat.out)
			*request->args.file_stat.out = g_async_stat;
		break;
	case FS_REQ_DIR_OPEN:
		if (request->args.dir_open.out)
			*request->args.dir_open.out = g_async_dir;
		break;
	case FS_REQ_DIR_READ:
		if (request->args.dir_read.entry)
			*request->args.dir_read.entry = g_async_dirent;
		break;
	case FS_REQ_PATH_STAT:
		if (request->args.path_stat.out)
			*request->args.path_stat.out = g_async_stat;
		break;
	default:
		break;
	}
	return g_async_request.result;
}

static void fs_server_admit(void)
{
	const uint64_t period_us = CONFIG_OVE_LINUX_FS_SERVER_PERIOD_US;
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	if (g_server_period_start_us == 0u || now < g_server_period_start_us ||
	    now - g_server_period_start_us >= period_us) {
		g_server_period_start_us = now;
		g_server_requests = 0;
	}
	if (g_server_requests != 0u) {
		uint64_t release = g_server_period_start_us + period_us;
		if (release > now)
			ove_time_delay_us((uint32_t)(release - now));
		(void)ove_time_get_us(&now);
		g_server_period_start_us = now;
		g_server_requests = 0;
	}
	g_server_requests++;
}

static void fs_worker(void *arg)
{
	(void)arg;
	for (;;) {
		struct fs_request *request = NULL;
		int rc = ove_queue_receive(g_request_queue, &request, OVE_WAIT_FOREVER);
		if (rc != OVE_OK)
			continue;
		if (request->op != FS_REQ_MOUNT && request->op != FS_REQ_STOP)
			fs_server_admit();
		(void)ove_time_get_us(&request->started_us);
		request->result = execute_request(request);
		(void)ove_time_get_us(&request->finished_us);
		request->budget_overrun =
			request->finished_us >= request->started_us &&
			request->finished_us - request->started_us >
				CONFIG_OVE_LINUX_FS_SERVER_BUDGET_US;
		int stop = request->op == FS_REQ_STOP;
		if (request->asynchronous) {
			if (__atomic_load_n(&g_async_cancelled, __ATOMIC_ACQUIRE)) {
				__atomic_store_n(&g_async_state, FS_ASYNC_IDLE, __ATOMIC_RELEASE);
			} else {
				__atomic_store_n(&g_async_state, FS_ASYNC_COMPLETE, __ATOMIC_RELEASE);
			}
			lxp_fs_kick();
		} else {
			ove_event_signal(g_complete);
		}
		if (stop)
			return;
	}
}

static int submit_sync(struct fs_request *request)
{
	if (!g_active || request == NULL)
		return LXP_ERR_INVALID_PARAM;
	int rc = ove_mutex_lock(g_submit_lock, OVE_WAIT_FOREVER);
	if (rc != OVE_OK)
		return rc;
	if (request->op == FS_REQ_FILE_WRITE || request->op == FS_REQ_FILE_PWRITE) {
		const void *source = request->op == FS_REQ_FILE_WRITE
					     ? request->args.file_write.buf
					     : request->args.file_pwrite.buf;
		size_t count = request->op == FS_REQ_FILE_WRITE
				       ? request->args.file_write.count
				       : request->args.file_pwrite.count;
		if (source == NULL || count > sizeof(g_io_buffer)) {
			ove_mutex_unlock(g_submit_lock);
			return LXP_ERR_INVALID_PARAM;
		}
		memcpy(g_io_buffer, source, count);
	} else if ((request->op == FS_REQ_FILE_READ &&
		    (request->args.file_read.buf == NULL ||
		     request->args.file_read.count > sizeof(g_io_buffer))) ||
		   (request->op == FS_REQ_FILE_PREAD &&
		    (request->args.file_pread.buf == NULL ||
		     request->args.file_pread.count > sizeof(g_io_buffer)))) {
		ove_mutex_unlock(g_submit_lock);
		return LXP_ERR_INVALID_PARAM;
	}
	int measured = request->op != FS_REQ_MOUNT && request->op != FS_REQ_STOP;
	request->submitted_us = 0;
	request->started_us = 0;
	request->finished_us = 0;
	if (measured) {
		(void)ove_time_get_us(&request->submitted_us);
		g_metrics.requests_submitted++;
		g_metrics.pending++;
		if (g_metrics.pending > g_metrics.queue_depth_max)
			g_metrics.queue_depth_max = g_metrics.pending;
	}
	struct fs_request *queued = request;
	rc = ove_queue_send(g_request_queue, &queued, OVE_WAIT_FOREVER);
	if (rc == OVE_OK)
		rc = ove_event_wait(g_complete, OVE_WAIT_FOREVER);
	if (rc == OVE_OK &&
	    (request->op == FS_REQ_FILE_READ || request->op == FS_REQ_FILE_PREAD)) {
		size_t transferred = request->op == FS_REQ_FILE_READ
					     ? request->args.file_read.transferred
					     : request->args.file_pread.transferred;
		size_t count = request->op == FS_REQ_FILE_READ ? request->args.file_read.count
							: request->args.file_pread.count;
		void *destination = request->op == FS_REQ_FILE_READ ? request->args.file_read.buf
							     : request->args.file_pread.buf;
		if (transferred > count) {
			rc = LXP_ERR_IO;
		} else if (transferred != 0u) {
			memcpy(destination, g_io_buffer, transferred);
		}
	}
	if (measured) {
		uint64_t queue_us = request->started_us >= request->submitted_us
					    ? request->started_us - request->submitted_us
					    : 0;
		uint64_t service_us = request->finished_us >= request->started_us
					      ? request->finished_us - request->started_us
					      : 0;
		g_metrics.pending--;
		if (rc == OVE_OK) {
			g_metrics.requests_completed++;
			if (request->result != LXP_OK && request->result != LXP_ERR_EOF)
				g_metrics.requests_failed++;
			if (request->op == FS_REQ_FILE_READ)
				g_metrics.bytes_read += request->args.file_read.transferred;
			else if (request->op == FS_REQ_FILE_PREAD)
				g_metrics.bytes_read += request->args.file_pread.transferred;
			else if (request->op == FS_REQ_FILE_WRITE)
				g_metrics.bytes_written += request->args.file_write.transferred;
			else if (request->op == FS_REQ_FILE_PWRITE)
				g_metrics.bytes_written += request->args.file_pwrite.transferred;
		} else {
			g_metrics.requests_failed++;
		}
		g_metrics.queue_wait_us_total += queue_us;
		if (queue_us > g_metrics.queue_wait_us_max)
			g_metrics.queue_wait_us_max = queue_us;
		g_metrics.service_us_total += service_us;
		if (service_us > g_metrics.service_us_max)
			g_metrics.service_us_max = service_us;
		if (request->budget_overrun)
			g_metrics.budget_overruns++;
	}
	ove_mutex_unlock(g_submit_lock);
	return rc == OVE_OK ? request->result : rc;
}

static void async_metrics_complete(const struct fs_request *request, int result)
{
	uint64_t queue_us = request->started_us >= request->submitted_us
				    ? request->started_us - request->submitted_us
				    : 0;
	uint64_t service_us = request->finished_us >= request->started_us
				      ? request->finished_us - request->started_us
				      : 0;
	if (g_metrics.pending)
		g_metrics.pending--;
	g_metrics.requests_completed++;
	if (result != LXP_OK && result != LXP_ERR_EOF)
		g_metrics.requests_failed++;
	if (request->op == FS_REQ_FILE_READ)
		g_metrics.bytes_read += request->args.file_read.transferred;
	else if (request->op == FS_REQ_FILE_PREAD)
		g_metrics.bytes_read += request->args.file_pread.transferred;
	else if (request->op == FS_REQ_FILE_WRITE)
		g_metrics.bytes_written += request->args.file_write.transferred;
	else if (request->op == FS_REQ_FILE_PWRITE)
		g_metrics.bytes_written += request->args.file_pwrite.transferred;
	g_metrics.queue_wait_us_total += queue_us;
	if (queue_us > g_metrics.queue_wait_us_max)
		g_metrics.queue_wait_us_max = queue_us;
	g_metrics.service_us_total += service_us;
	if (service_us > g_metrics.service_us_max)
		g_metrics.service_us_max = service_us;
	if (request->budget_overrun)
		g_metrics.budget_overruns++;
}

static int submit_async(struct fs_request *request)
{
	int state = __atomic_load_n(&g_async_state, __ATOMIC_ACQUIRE);
	if (state == FS_ASYNC_ACTIVE)
		return LXP_ERR_WOULD_BLOCK;
	if (state == FS_ASYNC_COMPLETE) {
		if (g_async_owner != g_selected_owner || request->op != g_async_request.op)
			return LXP_ERR_WOULD_BLOCK;
		int result = async_collect(request);
		async_metrics_complete(&g_async_request, result);
		g_async_owner = 0;
		__atomic_store_n(&g_async_state, FS_ASYNC_IDLE, __ATOMIC_RELEASE);
		return result;
	}

	if ((request->op == FS_REQ_FILE_WRITE || request->op == FS_REQ_FILE_PWRITE)) {
		const void *source = request->op == FS_REQ_FILE_WRITE
					     ? request->args.file_write.buf
					     : request->args.file_pwrite.buf;
		size_t count = request->op == FS_REQ_FILE_WRITE
				       ? request->args.file_write.count
				       : request->args.file_pwrite.count;
		if ((source == NULL && count != 0u) || count > sizeof(g_io_buffer))
			return LXP_ERR_INVALID_PARAM;
		if (count)
			memcpy(g_io_buffer, source, count);
	} else if ((request->op == FS_REQ_FILE_READ &&
		    (request->args.file_read.buf == NULL && request->args.file_read.count != 0u)) ||
		   (request->op == FS_REQ_FILE_PREAD &&
		    (request->args.file_pread.buf == NULL && request->args.file_pread.count != 0u))) {
		return LXP_ERR_INVALID_PARAM;
	}

	int rc = async_prepare(request);
	if (rc != LXP_OK)
		return rc;
	g_async_owner = g_selected_owner;
	__atomic_store_n(&g_async_cancelled, 0, __ATOMIC_RELEASE);
	(void)ove_time_get_us(&g_async_request.submitted_us);
	g_metrics.requests_submitted++;
	g_metrics.pending++;
	if (g_metrics.pending > g_metrics.queue_depth_max)
		g_metrics.queue_depth_max = g_metrics.pending;
	__atomic_store_n(&g_async_state, FS_ASYNC_ACTIVE, __ATOMIC_RELEASE);
	struct fs_request *queued = &g_async_request;
	rc = ove_queue_send(g_request_queue, &queued, 0);
	if (rc != OVE_OK) {
		__atomic_store_n(&g_async_state, FS_ASYNC_IDLE, __ATOMIC_RELEASE);
		g_metrics.pending--;
		g_metrics.requests_failed++;
		return rc;
	}
	return LXP_ERR_WOULD_BLOCK;
}

static int submit(struct fs_request *request)
{
	return g_selected_owner != 0u && request->op != FS_REQ_MOUNT && request->op != FS_REQ_STOP
		       ? submit_async(request)
		       : submit_sync(request);
}

static int fs_run_begin(void)
{
	struct fs_request request = {.op = FS_REQ_MOUNT};
	int rc;

	if (g_active)
		return LXP_ERR_WOULD_BLOCK;
	memset(g_handles, 0, sizeof(g_handles));
	memset(&g_metrics, 0, sizeof(g_metrics));
	memset(&g_async_request, 0, sizeof(g_async_request));
	g_selected_owner = 0;
	g_async_owner = 0;
	g_async_cancelled = 0;
	g_async_state = FS_ASYNC_IDLE;
	g_server_period_start_us = 0;
	g_server_requests = 0;
	g_mounted = 0;
	rc = ove_mutex_init(&g_submit_lock, &g_submit_lock_storage);
	if (rc != OVE_OK)
		return rc;
	rc = ove_event_init(&g_complete, &g_complete_storage);
	if (rc != OVE_OK)
		goto fail_event;
	rc = ove_queue_init(&g_request_queue, &g_request_queue_storage, g_request_queue_buffer,
			    sizeof(g_request_queue_buffer[0]), 1);
	if (rc != OVE_OK)
		goto fail_queue;
	/*
	 * The coordinator waits synchronously for this worker. It must therefore
	 * outrank every Linux guest: a second guest can reach its park trampoline
	 * while the coordinator is waiting and, with time slicing disabled, would
	 * otherwise starve a normal-priority worker indefinitely.
	 */
	rc = ove_thread_init(&g_worker, &g_worker_storage, "lxp-fs", fs_worker, NULL,
			     LXP_OVE_FS_WORKER_PRIORITY, sizeof(g_worker_stack), g_worker_stack);
	if (rc != OVE_OK)
		goto fail_thread;
	g_active = 1;
	/* Missing media is intentionally not a lifecycle failure. The worker retries
	 * mounting on the first subsequent operation, so hot insertion also works. */
	(void)submit(&request);
	return LXP_OK;

fail_thread:
	ove_queue_deinit(g_request_queue);
fail_queue:
	ove_event_deinit(g_complete);
fail_event:
	ove_mutex_deinit(g_submit_lock);
	return rc;
}

static void fs_run_end(void)
{
	struct fs_request request = {.op = FS_REQ_STOP};

	if (!g_active)
		return;
	g_selected_owner = 0;
	int rc = submit(&request);
	if (rc == LXP_OK)
		(void)ove_thread_deinit(g_worker);
	g_active = 0;
	ove_queue_deinit(g_request_queue);
	ove_event_deinit(g_complete);
	ove_mutex_deinit(g_submit_lock);
	memset(g_handles, 0, sizeof(g_handles));
}

static void fs_request_owner(uint64_t owner)
{
	g_selected_owner = owner;
}

static void fs_request_cancel(uint64_t owner)
{
	if (owner == 0u || owner != g_async_owner)
		return;
	int state = __atomic_load_n(&g_async_state, __ATOMIC_ACQUIRE);
	if (state == FS_ASYNC_COMPLETE) {
		async_metrics_complete(&g_async_request, LXP_ERR_WOULD_BLOCK);
		g_async_owner = 0;
		__atomic_store_n(&g_async_state, FS_ASYNC_IDLE, __ATOMIC_RELEASE);
	} else if (state == FS_ASYNC_ACTIVE) {
		__atomic_store_n(&g_async_cancelled, 1, __ATOMIC_RELEASE);
		if (g_metrics.pending)
			g_metrics.pending--;
		g_metrics.requests_failed++;
	}
}

static int fs_file_open(const char *path, unsigned int flags, lxp_fs_file_t *out)
{
	struct fs_request request = {
		.op = FS_REQ_FILE_OPEN,
		.args.file_open = {.path = path, .flags = flags, .out = out},
	};
	return submit(&request);
}

static int fs_object_open(const char *path, unsigned int flags, int require_dir,
			  lxp_fs_open_result_t *out)
{
	struct fs_request request = {
		.op = FS_REQ_OBJECT_OPEN,
		.args.object_open = {
			.path = path,
			.flags = flags,
			.require_dir = require_dir,
			.out = out,
		},
	};
	return submit(&request);
}

static int fs_file_close(lxp_fs_file_t file)
{
	struct fs_request request = {
		.op = FS_REQ_FILE_CLOSE,
		.args.file = {.file = file},
	};
	return submit(&request);
}

static int fs_file_read(lxp_fs_file_t file, void *buf, size_t count, size_t *bytes_read)
{
	uint8_t *destination = buf;
	size_t total = 0;

	if ((buf == NULL && count != 0u) || bytes_read == NULL)
		return LXP_ERR_INVALID_PARAM;
	*bytes_read = 0;
	while (total < count) {
		size_t chunk = count - total;
		if (chunk > sizeof(g_io_buffer))
			chunk = sizeof(g_io_buffer);
		struct fs_request request = {
			.op = FS_REQ_FILE_READ,
			.args.file_read =
				{
					.file = file,
					.buf = destination + total,
					.count = chunk,
				},
		};
		int rc = submit(&request);
		size_t received = request.args.file_read.transferred;
		if (received > chunk)
			return LXP_ERR_IO;
		total += received;
		if (rc != LXP_OK || received < chunk) {
			*bytes_read = total;
			return total != 0u ? LXP_OK : rc;
		}
	}
	*bytes_read = total;
	return LXP_OK;
}

static int fs_file_write(lxp_fs_file_t file, const void *buf, size_t count, size_t *bytes_written)
{
	const uint8_t *source = buf;
	size_t total = 0;

	if ((buf == NULL && count != 0u) || bytes_written == NULL)
		return LXP_ERR_INVALID_PARAM;
	*bytes_written = 0;
	while (total < count) {
		size_t chunk = count - total;
		if (chunk > sizeof(g_io_buffer))
			chunk = sizeof(g_io_buffer);
		struct fs_request request = {
			.op = FS_REQ_FILE_WRITE,
			.args.file_write =
				{
					.file = file,
					.buf = source + total,
					.count = chunk,
				},
		};
		int rc = submit(&request);
		size_t written = request.args.file_write.transferred;
		if (written > chunk)
			return LXP_ERR_IO;
		total += written;
		if (rc != LXP_OK || written < chunk) {
			*bytes_written = total;
			return total != 0u ? LXP_OK : rc;
		}
	}
	*bytes_written = total;
	return LXP_OK;
}

static int fs_file_pread(lxp_fs_file_t file, void *buf, size_t count, uint64_t offset,
			 size_t *bytes_read)
{
	uint8_t *destination = buf;
	size_t total = 0;

	if ((buf == NULL && count != 0u) || bytes_read == NULL)
		return LXP_ERR_INVALID_PARAM;
	*bytes_read = 0;
	while (total < count) {
		size_t chunk = count - total;
		if (chunk > sizeof(g_io_buffer))
			chunk = sizeof(g_io_buffer);
		struct fs_request request = {
			.op = FS_REQ_FILE_PREAD,
			.args.file_pread = {
				.file = file,
				.buf = destination + total,
				.count = chunk,
				.offset = offset + total,
			},
		};
		int rc = submit(&request);
		size_t received = request.args.file_pread.transferred;
		if (received > chunk)
			return LXP_ERR_IO;
		total += received;
		if (rc != LXP_OK || received < chunk) {
			*bytes_read = total;
			return total != 0u ? LXP_OK : rc;
		}
	}
	*bytes_read = total;
	return LXP_OK;
}

static int fs_file_pwrite(lxp_fs_file_t file, const void *buf, size_t count, uint64_t offset,
			  size_t *bytes_written)
{
	const uint8_t *source = buf;
	size_t total = 0;

	if ((buf == NULL && count != 0u) || bytes_written == NULL)
		return LXP_ERR_INVALID_PARAM;
	*bytes_written = 0;
	while (total < count) {
		size_t chunk = count - total;
		if (chunk > sizeof(g_io_buffer))
			chunk = sizeof(g_io_buffer);
		struct fs_request request = {
			.op = FS_REQ_FILE_PWRITE,
			.args.file_pwrite = {
				.file = file,
				.buf = source + total,
				.count = chunk,
				.offset = offset + total,
			},
		};
		int rc = submit(&request);
		size_t written = request.args.file_pwrite.transferred;
		if (written > chunk)
			return LXP_ERR_IO;
		total += written;
		if (rc != LXP_OK || written < chunk) {
			*bytes_written = total;
			return total != 0u ? LXP_OK : rc;
		}
	}
	*bytes_written = total;
	return LXP_OK;
}

static int fs_file_seek(lxp_fs_file_t file, int64_t offset, int whence, uint64_t *new_offset)
{
	struct fs_request request = {
		.op = FS_REQ_FILE_SEEK,
		.args.file_seek =
			{
				.file = file,
				.offset = offset,
				.whence = whence,
				.new_offset = new_offset,
			},
	};
	return submit(&request);
}

static int fs_file_stat(lxp_fs_file_t file, lxp_fs_stat_t *out)
{
	struct fs_request request = {
		.op = FS_REQ_FILE_STAT,
		.args.file_stat = {.file = file, .out = out},
	};
	return submit(&request);
}

static int fs_file_truncate(lxp_fs_file_t file, uint64_t length)
{
	struct fs_request request = {
		.op = FS_REQ_FILE_TRUNCATE,
		.args.file_truncate = {.file = file, .length = length},
	};
	return submit(&request);
}

static int fs_file_sync(lxp_fs_file_t file)
{
	struct fs_request request = {
		.op = FS_REQ_FILE_SYNC,
		.args.file = {.file = file},
	};
	return submit(&request);
}

static int fs_dir_open(const char *path, lxp_fs_dir_t *out)
{
	struct fs_request request = {
		.op = FS_REQ_DIR_OPEN,
		.args.dir_open = {.path = path, .out = out},
	};
	return submit(&request);
}

static int fs_dir_read(lxp_fs_dir_t dir, lxp_fs_dirent_t *entry)
{
	struct fs_request request = {
		.op = FS_REQ_DIR_READ,
		.args.dir_read = {.dir = dir, .entry = entry},
	};
	return submit(&request);
}

static int fs_dir_close(lxp_fs_dir_t dir)
{
	struct fs_request request = {
		.op = FS_REQ_DIR_CLOSE,
		.args.dir = {.dir = dir},
	};
	return submit(&request);
}

static int fs_path_stat(const char *path, lxp_fs_stat_t *out)
{
	struct fs_request request = {
		.op = FS_REQ_PATH_STAT,
		.args.path_stat = {.path = path, .out = out},
	};
	return submit(&request);
}

static int fs_path_mkdir(const char *path)
{
	struct fs_request request = {
		.op = FS_REQ_PATH_MKDIR,
		.args.path = {.path = path},
	};
	return submit(&request);
}

static int fs_path_rmdir(const char *path)
{
	struct fs_request request = {
		.op = FS_REQ_PATH_RMDIR,
		.args.path = {.path = path},
	};
	return submit(&request);
}

static int fs_path_unlink(const char *path)
{
	struct fs_request request = {
		.op = FS_REQ_PATH_UNLINK,
		.args.path = {.path = path},
	};
	return submit(&request);
}

static int fs_path_rename(const char *old_path, const char *new_path)
{
	struct fs_request request = {
		.op = FS_REQ_PATH_RENAME,
		.args.path_rename = {.old_path = old_path, .new_path = new_path},
	};
	return submit(&request);
}

static int fs_metrics(lxp_fs_metrics_t *out)
{
	if (out == NULL)
		return LXP_ERR_INVALID_PARAM;
	*out = g_metrics;
	return g_active ? LXP_OK : LXP_ERR_NOT_REGISTERED;
}

const lxp_fs_ops_t g_lxp_host_fs_ops = {
	.abi_version = LXP_FS_OPS_ABI_VERSION,
	.struct_size = sizeof(lxp_fs_ops_t),
	.run_begin = fs_run_begin,
	.run_end = fs_run_end,
	.request_owner = fs_request_owner,
	.request_cancel = fs_request_cancel,
	.file_open = fs_file_open,
	.object_open = fs_object_open,
	.file_close = fs_file_close,
	.file_read = fs_file_read,
	.file_write = fs_file_write,
	.file_seek = fs_file_seek,
	.file_stat = fs_file_stat,
	.file_truncate = fs_file_truncate,
	.file_sync = fs_file_sync,
	.file_pread = fs_file_pread,
	.file_pwrite = fs_file_pwrite,
	.dir_open = fs_dir_open,
	.dir_read = fs_dir_read,
	.dir_close = fs_dir_close,
	.path_stat = fs_path_stat,
	.path_mkdir = fs_path_mkdir,
	.path_rmdir = fs_path_rmdir,
	.path_unlink = fs_path_unlink,
	.path_rename = fs_path_rename,
	.metrics = fs_metrics,
};

#endif /* CONFIG_OVE_LINUX_FS */
