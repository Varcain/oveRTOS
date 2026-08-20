/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Consumer-side smoke tests for the oveRTOS/LXP host adapters. The network
 * adapter owns socket storage and bridges the module's handle API to ove_net.
 * The thread adapter copies snapshots across the two independently owned type
 * contracts without relying on compatible struct layout.
 */

#include "../framework/ove_test.h"

#include "ove/lxp_host.h"
#include "ove/lxp_console.h"
#include "ove/lxp_metrics.h"
#include "ove/thread.h"
#include "ove_net_ready.h"
#include "lxp_ove_thread_adapter.h"
#include "lxp/lxp_config.h"
#include "lxp/lxp_net_ops.h"
#include "ove/types.h" /* OVE_OK — the ove_net return the adapter forwards */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const struct lxp_net_ops g_lxp_host_net_ops;
extern const lxp_fs_ops_t g_lxp_host_fs_ops;
extern const lxp_block_ops_t g_lxp_host_block_ops;
static unsigned g_socket_kicks;
static const void *g_socket_ready_context;
static unsigned g_fs_kicks;
static unsigned g_block_kicks;
static unsigned g_host_init_calls;
static lxp_host_t *g_host_init_target;
static lxp_host_config_t g_host_init_config;
static lxp_netfs_config_t g_host_netfs_config;
static char g_host_netfs_mountpoint[LXP_NETFS_MOUNTPOINT_CAP];
static char g_host_netfs_aname[LXP_NETFS_ANAME_CAP];
static char g_host_netfs_uname[LXP_NETFS_UNAME_CAP];
static unsigned g_host_run_calls;
static const lxp_host_t *g_host_run_target;
static lxp_launch_config_t g_host_run_config;
static unsigned g_guest_exit_calls;
static ove_lxp_guest_exit_info_t g_guest_exit_info;

const lxp_os_ops_t g_lxp_host_engine = {
	.abi_version = LXP_OS_OPS_ABI_VERSION,
	.struct_size = sizeof(lxp_os_ops_t),
};

const lxp_display_ops_t g_lxp_host_display_ops = {
	.abi_version = LXP_DISPLAY_OPS_ABI_VERSION,
	.struct_size = sizeof(lxp_display_ops_t),
};

int lxp_host_init_cpio(lxp_host_t *host, const lxp_host_config_t *config)
{
	g_host_init_calls++;
	g_host_init_target = host;
	g_host_init_config = *config;
	if (config->netfs_config) {
		g_host_netfs_config = *config->netfs_config;
		strcpy(g_host_netfs_mountpoint, config->netfs_config->mountpoint);
		strcpy(g_host_netfs_aname, config->netfs_config->aname);
		strcpy(g_host_netfs_uname, config->netfs_config->uname);
		g_host_netfs_config.mountpoint = g_host_netfs_mountpoint;
		g_host_netfs_config.aname = g_host_netfs_aname;
		g_host_netfs_config.uname = g_host_netfs_uname;
		g_host_init_config.netfs_config = &g_host_netfs_config;
	}
	return LXP_OK;
}

int lxp_host_run(const lxp_host_t *host, const lxp_launch_config_t *config,
		 const char *path, int argc, const char *const argv[])
{
	(void)path;
	(void)argc;
	(void)argv;
	g_host_run_calls++;
	g_host_run_target = host;
	memset(&g_host_run_config, 0, sizeof(g_host_run_config));
	if (config) {
		g_host_run_config = *config;
		if (config->on_guest_exit) {
			const lxp_guest_exit_info_t info = {
				.slot = 3,
				.pid = 27,
				.ppid = 7,
				.status = 139,
				.comm = "faulty",
				.reason = LXP_EXIT_REASON_STATE_CORRUPTION,
				.signal = 11,
				.detail = 0x1234u,
				.address = 0x5678u,
			};
			config->on_guest_exit(config->guest_exit_ctx, &info);
		}
	}
	return 37;
}

static void test_socket_ready(const void *context)
{
	g_socket_kicks++;
	g_socket_ready_context = context;
}

void lxp_block_kick(void)
{
	g_block_kicks++;
}

void lxp_fs_kick(void)
{
	__atomic_add_fetch(&g_fs_kicks, 1u, __ATOMIC_RELAXED);
}

/* The exported adapter table passed to lxp_run must be complete and versioned. */
static void test_adapter_ops_wired(void **state)
{
	(void)state;
	const struct lxp_net_ops *ops = &g_lxp_host_net_ops;
	assert_non_null(ops);
	assert_int_equal(ops->abi_version, LXP_NET_OPS_ABI_VERSION);
	assert_int_equal(ops->struct_size, sizeof(*ops));
	assert_non_null(ops->run_begin);
	assert_non_null(ops->run_end);
	assert_non_null(ops->sock_open);
	assert_non_null(ops->sock_accept);
	assert_non_null(ops->sock_close);
	assert_non_null(ops->sock_connect);
	assert_non_null(ops->sock_bind);
	assert_non_null(ops->sock_listen);
	assert_non_null(ops->sock_send);
	assert_non_null(ops->sock_recv);
	assert_non_null(ops->sock_sendto);
	assert_non_null(ops->sock_recvfrom);
	assert_non_null(ops->sock_set_nonblock);
	assert_non_null(ops->sock_poll);
	assert_non_null(ops->sock_shutdown);
	assert_non_null(ops->sock_getsockname);
	assert_non_null(ops->sock_getpeername);
	assert_non_null(ops->sock_get_error);
	assert_non_null(ops->netif_get_addr);
	assert_non_null(ops->netif_get_hwaddr);
	assert_non_null(ops->netif_get_flags);
	assert_non_null(ops->netif_set_addr);
	assert_non_null(ops->netif_set_up);
	assert_true(ops->capabilities & LXP_NET_CAP_SOCKET_READY_EVENT);
}

static void test_fs_adapter_ops_wired(void **state)
{
	(void)state;
	const lxp_fs_ops_t *ops = &g_lxp_host_fs_ops;
	assert_int_equal(ops->abi_version, LXP_FS_OPS_ABI_VERSION);
	assert_int_equal(ops->struct_size, sizeof(*ops));
	assert_non_null(ops->run_begin);
	assert_non_null(ops->run_end);
	assert_non_null(ops->request_owner);
	assert_non_null(ops->request_cancel);
	assert_non_null(ops->mount);
	assert_non_null(ops->unmount);
	assert_non_null(ops->is_mounted);
	assert_non_null(ops->volume_stat);
	assert_non_null(ops->file_open);
	assert_non_null(ops->object_open);
	assert_non_null(ops->file_close);
	assert_non_null(ops->file_read);
	assert_non_null(ops->file_write);
	assert_non_null(ops->file_seek);
	assert_non_null(ops->file_stat);
	assert_non_null(ops->file_truncate);
	assert_non_null(ops->file_sync);
	assert_non_null(ops->file_pread);
	assert_non_null(ops->file_pwrite);
	assert_non_null(ops->dir_open);
	assert_non_null(ops->dir_read);
	assert_non_null(ops->dir_close);
	assert_non_null(ops->path_stat);
	assert_non_null(ops->path_mkdir);
	assert_non_null(ops->path_rmdir);
	assert_non_null(ops->path_unlink);
	assert_non_null(ops->path_rename);
	assert_non_null(ops->metrics);
}

static void test_block_adapter_media_arbitration(void **state)
{
	(void)state;
	const lxp_fs_ops_t *fs = &g_lxp_host_fs_ops;
	const lxp_block_ops_t *block = &g_lxp_host_block_ops;
	char image[] = "/tmp/ove-lxp-block-XXXXXX";
	int fd = mkstemp(image);
	assert_true(fd >= 0);
	assert_int_equal(ftruncate(fd, 8192), 0);
	assert_int_equal(close(fd), 0);
	assert_int_equal(setenv("OVE_BLOCK_IMAGE", image, 1), 0);

	assert_int_equal(fs->run_begin(), LXP_OK);
	assert_int_equal(block->run_begin(), LXP_OK);
	assert_int_equal(fs->run_begin(), LXP_ERR_WOULD_BLOCK);
	assert_int_equal(block->run_begin(), LXP_ERR_WOULD_BLOCK);
	lxp_block_info_t info = {0};
	assert_int_equal(block->get_info(&info), LXP_OK);
	assert_int_equal(info.block_count, 16);
	assert_int_equal(info.logical_block_size, 512);
	assert_int_equal(info.erase_block_size, 512);

	/* Read-only inspection can coexist with /data. Writable raw ownership cannot. */
	assert_int_equal(block->open(0), LXP_OK);
	block->close(0);
	assert_int_equal(block->open(LXP_BLOCK_OPEN_WRITE), LXP_ERR_BUSY);
	assert_int_equal(fs->unmount(), LXP_OK);
	assert_false(fs->is_mounted());
	assert_int_equal(block->open(LXP_BLOCK_OPEN_WRITE), LXP_OK);
	lxp_fs_mount_spec_t volume = {
		.block_count = info.block_count,
		.logical_block_size = info.logical_block_size,
	};
	assert_int_equal(fs->mount(&volume), LXP_ERR_BUSY);

	/* Cross-sector byte I/O proves the adapter's bounded read/modify/write path. */
	const uint8_t written[] = {0x12, 0x34, 0x56};
	uint8_t readback[sizeof(written)] = {0};
	size_t done = 0;
	assert_int_equal(block->write(511, written, sizeof(written), &done), LXP_OK);
	assert_int_equal(done, sizeof(written));
	assert_int_equal(block->read(511, readback, sizeof(readback), &done), LXP_OK);
	assert_int_equal(done, sizeof(readback));
	assert_memory_equal(readback, written, sizeof(written));
	assert_int_equal(block->sync(), LXP_OK);
	block->close(LXP_BLOCK_OPEN_WRITE); /* Last writable close also syncs. */

	assert_int_equal(fs->mount(&volume), LXP_OK);
	assert_true(fs->is_mounted());
	block->run_end();
	fs->run_end();
	assert_int_equal(unsetenv("OVE_BLOCK_IMAGE"), 0);
	assert_int_equal(unlink(image), 0);
}

static void test_fs_adapter_serialized_lifecycle(void **state)
{
	(void)state;
	const lxp_fs_ops_t *ops = &g_lxp_host_fs_ops;
	char base[] = "/tmp/ove-lxp-fs-XXXXXX";
	char file[128];
	char moved[128];
	char child[128];
	char readback[8] = {0};
	const char payload[] = "hello";
	lxp_fs_file_t handle = NULL;
	lxp_fs_dir_t dir = NULL;
	lxp_fs_stat_t stat;
	lxp_fs_dirent_t entry;
	size_t done = 0;
	uint64_t offset = UINT64_MAX;
	int found = 0;

	assert_non_null(mkdtemp(base));
	assert_true(snprintf(file, sizeof(file), "%s/file", base) > 0);
	assert_true(snprintf(moved, sizeof(moved), "%s/moved", base) > 0);
	assert_true(snprintf(child, sizeof(child), "%s/child", base) > 0);

	assert_int_equal(ops->run_begin(), LXP_OK);
	assert_int_equal(ops->run_begin(), LXP_ERR_WOULD_BLOCK);
	assert_int_equal(ops->file_open(file,
					LXP_FS_O_READ | LXP_FS_O_WRITE | LXP_FS_O_CREATE |
						LXP_FS_O_TRUNC,
					&handle),
			 LXP_OK);
	assert_non_null(handle);
	assert_int_equal(ops->file_write(handle, payload, sizeof(payload) - 1u, &done), LXP_OK);
	assert_int_equal(done, sizeof(payload) - 1u);
	assert_int_equal(ops->file_seek(handle, 0, LXP_FS_SEEK_SET, &offset), LXP_OK);
	assert_int_equal(offset, 0);
	assert_int_equal(ops->file_read(handle, readback, sizeof(payload) - 1u, &done), LXP_OK);
	assert_int_equal(done, sizeof(payload) - 1u);
	assert_memory_equal(readback, payload, sizeof(payload) - 1u);
	assert_int_equal(ops->file_stat(handle, &stat), LXP_OK);
	assert_int_equal(stat.type, LXP_FS_TYPE_FILE);
	assert_int_equal(stat.size, sizeof(payload) - 1u);
	assert_int_equal(ops->file_truncate(handle, 3), LXP_OK);
	assert_int_equal(ops->file_sync(handle), LXP_OK);
	assert_int_equal(ops->file_close(handle), LXP_OK);
	assert_int_equal(ops->file_close(handle), LXP_ERR_BAD_HANDLE);

	assert_int_equal(ops->path_rename(file, moved), LXP_OK);
	assert_int_equal(ops->path_stat(moved, &stat), LXP_OK);
	assert_int_equal(stat.size, 3);
	assert_int_equal(ops->dir_open(base, &dir), LXP_OK);
	while (ops->dir_read(dir, &entry) == LXP_OK)
		if (strcmp(entry.name, "moved") == 0)
			found = 1;
	assert_true(found);
	assert_int_equal(ops->dir_close(dir), LXP_OK);
	assert_int_equal(ops->path_mkdir(child), LXP_OK);
	assert_int_equal(ops->path_rmdir(child), LXP_OK);
	assert_int_equal(ops->path_unlink(moved), LXP_OK);
	assert_int_equal(ops->path_rmdir(base), LXP_OK);
	ops->run_end();

	assert_int_equal(ops->path_stat(base, &stat), LXP_ERR_INVALID_PARAM);
	assert_int_equal(ops->run_begin(), LXP_OK);
	assert_int_equal(ops->file_open("/tmp/ove-lxp-fs-leaked",
					LXP_FS_O_WRITE | LXP_FS_O_CREATE | LXP_FS_O_TRUNC, &handle),
			 LXP_OK);
	ops->run_end(); /* Provider teardown owns leaked native handles. */
	assert_int_equal(ops->run_begin(), LXP_OK);
	assert_int_equal(ops->path_unlink("/tmp/ove-lxp-fs-leaked"), LXP_OK);
	ops->run_end();
}

/* A non-zero owner selects the coordinator-facing asynchronous path. The
 * first call submits work and parks; the wake hook tells the coordinator to
 * retry, and that retry collects the completed native result. */
static void test_fs_adapter_async_completion(void **state)
{
	(void)state;
	const lxp_fs_ops_t *ops = &g_lxp_host_fs_ops;
	char path[] = "/tmp/ove-lxp-fs-async-XXXXXX";
	lxp_fs_file_t handle = NULL;
	int fd = mkstemp(path);
	assert_true(fd >= 0);
	assert_int_equal(close(fd), 0);

	assert_int_equal(ops->run_begin(), LXP_OK);
	/* Cancel completed opens beyond the native slot count. A leaked orphan
	 * would exhaust the pool before this loop or the final open can finish. */
	for (unsigned int owner = 100u; owner < 100u + LXP_NHOSTFS_OPEN + 2u; owner++) {
		ops->request_owner(owner);
		__atomic_store_n(&g_fs_kicks, 0u, __ATOMIC_RELAXED);
		assert_int_equal(ops->file_open(path, LXP_FS_O_READ, &handle),
				 LXP_ERR_WOULD_BLOCK);
		for (unsigned int i = 0;
		     i < 100000u && __atomic_load_n(&g_fs_kicks, __ATOMIC_RELAXED) == 0u;
		     i++)
			ove_thread_yield();
		assert_int_equal(__atomic_load_n(&g_fs_kicks, __ATOMIC_RELAXED), 1u);
		ops->request_cancel(owner);
	}

	ops->request_owner(42u);
	__atomic_store_n(&g_fs_kicks, 0u, __ATOMIC_RELAXED);
	assert_int_equal(ops->file_open(path, LXP_FS_O_READ, &handle),
			 LXP_ERR_WOULD_BLOCK);
	for (unsigned int i = 0;
	     i < 100000u && __atomic_load_n(&g_fs_kicks, __ATOMIC_RELAXED) == 0u;
	     i++)
		ove_thread_yield();
	assert_int_equal(__atomic_load_n(&g_fs_kicks, __ATOMIC_RELAXED), 1u);
	assert_int_equal(ops->file_open(path, LXP_FS_O_READ, &handle), LXP_OK);
	assert_non_null(handle);

	ops->request_owner(0u);
	assert_int_equal(ops->file_close(handle), LXP_OK);
	assert_int_equal(ops->path_unlink(path), LXP_OK);
	ops->run_end();
}

/* Cancellation and worker completion race through the LXP gate. Whichever
 * wins must retire the old owner, reclaim an orphaned native open result, and
 * permit a new generation-qualified owner to make progress. */
static void test_fs_adapter_async_cancel_retires_owner(void **state)
{
	(void)state;
	const lxp_fs_ops_t *ops = &g_lxp_host_fs_ops;
	char path[] = "/tmp/ove-lxp-fs-cancel-XXXXXX";
	lxp_fs_file_t handle = NULL;
	int fd = mkstemp(path);
	assert_true(fd >= 0);
	assert_int_equal(close(fd), 0);

	assert_int_equal(ops->run_begin(), LXP_OK);
	ops->request_owner(42u);
	assert_int_equal(ops->file_open(path, LXP_FS_O_READ, &handle), LXP_ERR_WOULD_BLOCK);
	ops->request_cancel(42u);
	__atomic_store_n(&g_fs_kicks, 0u, __ATOMIC_RELAXED);
	ops->request_owner(43u);
	int collected = 0;
	for (unsigned int i = 0; i < 1000000u; i++) {
		int rc = ops->file_open(path, LXP_FS_O_READ, &handle);
		if (rc == LXP_OK) {
			collected = 1;
			break;
		}
		assert_int_equal(rc, LXP_ERR_WOULD_BLOCK);
		ove_thread_yield();
	}
	assert_true(collected);
	assert_non_null(handle);

	ops->request_owner(0u);
	assert_int_equal(ops->file_close(handle), LXP_OK);
	assert_int_equal(ops->path_unlink(path), LXP_OK);
	ops->run_end();
}

/* open -> close through the adapter reaches the ove_net backend (posix_net):
 * exercises the storage-pool slot alloc/free and the ove_socket_open_ex bridge.
 * Readiness remains subscribed until the last concurrently owned socket closes. */
static void test_adapter_open_close(void **state)
{
	(void)state;
	const struct lxp_net_ops *ops = &g_lxp_host_net_ops;
	static const unsigned ready_context;
	assert_int_equal(ops->run_begin(NULL, NULL), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ops->run_begin(test_socket_ready, &ready_context), OVE_OK);
	assert_int_equal(ops->run_begin(test_socket_ready, &ready_context), OVE_ERR_WOULD_BLOCK);

	lxp_socket_t s = NULL;
	int rc = ops->sock_open(LXP_AF_INET, LXP_SOCK_DGRAM, 0, &s);
	if (rc == OVE_ERR_NOT_SUPPORTED) {
		ops->run_end();
		print_message("[  SKIP  ] lxp adapter sockets denied by test sandbox\n");
		skip();
	}
	assert_int_equal(rc, OVE_OK);
	assert_non_null(s);
	lxp_socket_t t = NULL;
	assert_int_equal(ops->sock_open(LXP_AF_INET, LXP_SOCK_STREAM, 0, &t), OVE_OK);
	assert_non_null(t);

	g_socket_kicks = 0;
	g_socket_ready_context = NULL;
	ove_net_ready_publish();
	assert_int_equal(g_socket_kicks, 1);
	assert_ptr_equal(g_socket_ready_context, &ready_context);
	ops->sock_close(s);
	ove_net_ready_publish();
	assert_int_equal(g_socket_kicks, 2);
	ops->sock_close(t);
	ove_net_ready_publish();
	assert_int_equal(g_socket_kicks, 2);
	unsigned revents = 0;
	assert_int_equal(ops->sock_poll(t, 0, &revents, 0), OVE_ERR_INVALID_PARAM);
	ops->run_end();
	assert_int_equal(ops->sock_open(LXP_AF_INET, LXP_SOCK_DGRAM, 0, &s), OVE_ERR_INVALID_PARAM);

	/* A run-end owns rollback for a provider handle the core failed to close. */
	assert_int_equal(ops->run_begin(test_socket_ready, &ready_context), OVE_OK);
	assert_int_equal(ops->sock_open(LXP_AF_INET, LXP_SOCK_DGRAM, 0, &s), OVE_OK);
	ops->run_end();
	ove_net_ready_publish();
	assert_int_equal(g_socket_kicks, 2);
	assert_int_equal(ops->run_begin(test_socket_ready, &ready_context), OVE_OK);
	ops->run_end();
}

static long test_launch_write(void *ctx, int fd, const void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	(void)buf;
	return (long)len;
}

static long test_launch_read(void *ctx, int fd, void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	(void)buf;
	(void)len;
	return 0;
}

static void test_enosys(long nr)
{
	(void)nr;
}

static long test_rt_scope_read(void *ctx, char *buf, size_t cap)
{
	(void)ctx;
	(void)buf;
	(void)cap;
	return 0;
}

static void test_guest_exit(const ove_lxp_guest_exit_info_t *info)
{
	g_guest_exit_calls++;
	g_guest_exit_info = *info;
}

static void test_host_facade_owns_composition(void **state)
{
	(void)state;
	static const uint8_t rootfs[16];
	ove_lxp_host_t host;
	int io_cookie;
	const char *const env[] = {"PATH=/bin", NULL};
	const ove_lxp_launch_config_t config = {
		.write_fn = test_launch_write,
		.read_fn = test_launch_read,
		.io_ctx = &io_cookie,
		.on_enosys = test_enosys,
		.env = env,
		.on_guest_exit = test_guest_exit,
		.display_width = 800,
		.display_height = 480,
		.rt_scope_read = test_rt_scope_read,
		.rt_scope_ctx = &host,
	};
	const char *const argv[] = {"init", NULL};
	const ove_lxp_netfs_config_t netfs = {
		.mountpoint = "/mnt/pi",
		.server_ipv4 = "172.1.1.1",
		.port = 564,
		.aname = "/srv/pi9",
		.uname = "root",
	};
	ove_lxp_host_config_t host_config = {
		.rootfs_image = rootfs,
		.rootfs_image_size = sizeof(rootfs),
		.netfs_config = &netfs,
	};

	memset(&host, 0xa5, sizeof(host));
	g_host_init_calls = 0;
	g_host_init_target = NULL;
	memset(&g_host_init_config, 0, sizeof(g_host_init_config));
	assert_int_equal(ove_lxp_host_init_cpio(&host, &host_config), OVE_OK);
	assert_int_equal(g_host_init_calls, 1);
	assert_ptr_equal(g_host_init_target, &host.core);
	assert_ptr_equal(g_host_init_config.os_ops, &g_lxp_host_engine);
	assert_ptr_equal(g_host_init_config.net_ops, &g_lxp_host_net_ops);
	assert_ptr_equal(g_host_init_config.display_ops, &g_lxp_host_display_ops);
	assert_ptr_equal(g_host_init_config.fs_ops, &g_lxp_host_fs_ops);
	assert_ptr_equal(g_host_init_config.block_ops, &g_lxp_host_block_ops);
	assert_ptr_equal(g_host_init_config.rootfs_image, rootfs);
	assert_int_equal(g_host_init_config.rootfs_image_size, sizeof(rootfs));
	assert_ptr_equal(g_host_init_config.rootfs_storage, host.rootfs_files);
	assert_int_equal(g_host_init_config.rootfs_capacity, OVE_LXP_ROOTFS_FILE_CAPACITY);
	assert_ptr_equal(g_host_init_config.rootfs_name_storage, host.rootfs_names);
	assert_int_equal(g_host_init_config.rootfs_name_capacity, OVE_LXP_ROOTFS_NAME_CAPACITY);
	/* Host init resets live state, not the potentially large workspace. */
	assert_int_equal((unsigned char)host.rootfs_names[0], 0xa5);
	assert_null(g_host_init_config.netif);
	assert_non_null(g_host_init_config.netfs_config);
	assert_string_equal(g_host_init_config.netfs_config->mountpoint, "/mnt/pi");
	assert_memory_equal(g_host_init_config.netfs_config->server_ip,
			    ((const uint8_t[]){172, 1, 1, 1}), 4);
	assert_int_equal(g_host_init_config.netfs_config->port, 564);
	assert_string_equal(g_host_init_config.netfs_config->aname, "/srv/pi9");
	assert_string_equal(g_host_init_config.netfs_config->uname, "root");

	g_host_run_calls = 0;
	g_host_run_target = NULL;
	memset(&g_host_run_config, 0, sizeof(g_host_run_config));
	g_guest_exit_calls = 0;
	memset(&g_guest_exit_info, 0, sizeof(g_guest_exit_info));
	assert_int_equal(ove_lxp_host_run(&host, &config, "/init", 1, argv), 37);
	assert_int_equal(g_host_run_calls, 1);
	assert_ptr_equal(g_host_run_target, &host.core);
	assert_ptr_equal(g_host_run_config.write_fn, test_launch_write);
	assert_ptr_equal(g_host_run_config.read_fn, test_launch_read);
	assert_ptr_equal(g_host_run_config.io_ctx, &io_cookie);
	assert_ptr_equal(g_host_run_config.on_enosys, test_enosys);
	assert_ptr_equal(g_host_run_config.env, env);
	assert_non_null(g_host_run_config.on_guest_exit);
	assert_ptr_equal(g_host_run_config.guest_exit_ctx, &config);
	assert_int_equal(g_host_run_config.display_width, 800);
	assert_int_equal(g_host_run_config.display_height, 480);
	assert_ptr_equal(g_host_run_config.rt_scope_read, test_rt_scope_read);
	assert_ptr_equal(g_host_run_config.rt_scope_ctx, &host);
	assert_int_equal(g_guest_exit_calls, 1);
	assert_int_equal(g_guest_exit_info.slot, 3);
	assert_int_equal(g_guest_exit_info.pid, 27);
	assert_string_equal(g_guest_exit_info.comm, "faulty");
	assert_int_equal(g_guest_exit_info.reason, OVE_LXP_EXIT_REASON_STATE_CORRUPTION);
	assert_int_equal(g_guest_exit_info.detail, 0x1234u);
	assert_int_equal(g_guest_exit_info.address, 0x5678u);
	host.rootfs_names[0] = 'x';
	ove_lxp_host_deinit(&host);
	assert_int_equal(host.rootfs_names[0], 'x');

	host_config.netfs_config = &(const ove_lxp_netfs_config_t){
		.mountpoint = "/mnt/pi",
		.server_ipv4 = "172.1.1.999",
		.port = 564,
	};
	assert_int_equal(ove_lxp_host_init_cpio(&host, &host_config), OVE_ERR_INVALID_PARAM);
	assert_int_equal(g_host_init_calls, 1);
}

static void test_console_adapter_binds_only_console_policy(void **state)
{
	(void)state;
	int diagnostic_cookie;
	ove_lxp_launch_config_t config = {
		.on_enosys = test_enosys,
		.rt_scope_ctx = &diagnostic_cookie,
	};
	assert_int_equal(ove_lxp_console_init(), OVE_OK);
	ove_lxp_console_bind(&config);
	assert_non_null(config.read_fn);
	assert_non_null(config.write_fn);
	assert_non_null(config.console_poll);
	assert_null(config.io_ctx);
	/* The POSIX stub has no asynchronous RX source, so it deliberately retains
	 * LXP's bounded polling fallback. */
	assert_null(config.console_subscribe);
	assert_null(config.console_unsubscribe);
	assert_int_equal(config.console_poll(config.io_ctx), 0);
	assert_ptr_equal(config.on_enosys, test_enosys);
	assert_ptr_equal(config.rt_scope_ctx, &diagnostic_cookie);
}

static int32_t test_slot_lookup(uintptr_t identity)
{
	return identity == 0x1234u ? 3 : LXP_THREAD_SLOT_NONE;
}

static void test_thread_adapter_copies_contract(void **state)
{
	(void)state;
	const struct ove_thread_info host = {
		.name = "worker",
		.identity = 0x1234u,
		.lxp_slot = 99,
		.state = OVE_THREAD_STATE_BLOCKED,
		.priority = 7,
		.stack_used = 320,
		.stack_size = 1024,
		.cpu_percent_x100 = 1250,
		.state_times = {
			.running_us = 100,
			.ready_us = 200,
			.blocked_us = 300,
			.suspended_us = 400,
		},
	};
	struct lxp_thread_info out = {0};

	lxp_ove_thread_info_copy(&out, &host, test_slot_lookup);

	assert_ptr_equal(out.name, host.name);
	assert_int_equal(out.identity, host.identity);
	assert_int_equal(out.lxp_slot, 3);
	assert_int_equal(out.state, LXP_THREAD_STATE_BLOCKED);
	assert_int_equal(out.priority, host.priority);
	assert_int_equal(out.stack_used, host.stack_used);
	assert_int_equal(out.stack_size, host.stack_size);
	assert_int_equal(out.cpu_percent_x100, host.cpu_percent_x100);
	assert_int_equal(out.state_times.running_us, host.state_times.running_us);
	assert_int_equal(out.state_times.ready_us, host.state_times.ready_us);
	assert_int_equal(out.state_times.blocked_us, host.state_times.blocked_us);
	assert_int_equal(out.state_times.suspended_us, host.state_times.suspended_us);
}

static void test_thread_adapter_maps_unknown_state(void **state)
{
	(void)state;
	const struct ove_thread_info host = {
		.identity = 0x5678u,
		.state = (ove_thread_state_t)99,
	};
	struct lxp_thread_info out = {0};

	lxp_ove_thread_info_copy(&out, &host, test_slot_lookup);

	assert_int_equal(out.lxp_slot, LXP_THREAD_SLOT_NONE);
	assert_int_equal(out.state, LXP_THREAD_STATE_UNKNOWN);
}

static void test_svc_metrics_own_window_and_lifetime(void **state)
{
	(void)state;
	struct ove_lxp_svc_metrics window;
	struct ove_lxp_svc_metrics total;

	ove_lxp_svc_metrics_record(7u, 40u);
	ove_lxp_svc_metrics_record(8u, 20u);
	ove_lxp_svc_metrics_record(9u, 60u);
	ove_lxp_svc_metrics_take(&window, &total);

	assert_int_equal(window.calls, 3u);
	assert_int_equal(window.min_cycles, 20u);
	assert_int_equal(window.max_cycles, 60u);
	assert_int_equal(window.total_cycles, 120u);
	assert_int_equal(window.max_syscall, 9u);
	assert_int_equal(total.calls, window.calls);
	assert_int_equal(total.min_cycles, window.min_cycles);
	assert_int_equal(total.max_cycles, window.max_cycles);
	assert_int_equal(total.total_cycles, window.total_cycles);
	assert_int_equal(total.max_syscall, window.max_syscall);
	struct ove_lxp_svc_metrics snapshot;
	ove_lxp_svc_metrics_snapshot(&snapshot);
	assert_int_equal(snapshot.calls, total.calls);
	assert_int_equal(snapshot.min_cycles, total.min_cycles);
	assert_int_equal(snapshot.max_cycles, total.max_cycles);
	assert_int_equal(snapshot.total_cycles, total.total_cycles);
	assert_int_equal(snapshot.max_syscall, total.max_syscall);

	ove_lxp_svc_metrics_take(&window, &total);
	assert_int_equal(window.calls, 0u);
	assert_int_equal(total.calls, 3u);
	assert_int_equal(total.total_cycles, 120u);
}

int test_lxp_adapter_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_adapter_ops_wired),
		cmocka_unit_test(test_fs_adapter_ops_wired),
		cmocka_unit_test(test_block_adapter_media_arbitration),
		cmocka_unit_test(test_fs_adapter_serialized_lifecycle),
		cmocka_unit_test(test_fs_adapter_async_completion),
		cmocka_unit_test(test_fs_adapter_async_cancel_retires_owner),
		cmocka_unit_test(test_adapter_open_close),
		cmocka_unit_test(test_host_facade_owns_composition),
		cmocka_unit_test(test_console_adapter_binds_only_console_policy),
		cmocka_unit_test(test_thread_adapter_copies_contract),
		cmocka_unit_test(test_thread_adapter_maps_unknown_state),
		cmocka_unit_test(test_svc_metrics_own_window_and_lifetime),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
