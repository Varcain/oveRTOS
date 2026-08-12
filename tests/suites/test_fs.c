/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

#ifndef CONFIG_OVE_ZERO_HEAP

static int fs_open(ove_file_t *f, const char *path, int flags)
{
	return ove_fs_open(f, path, flags);
}

static int fs_close(ove_file_t f)
{
	return ove_fs_close(f);
}

static int fs_opendir(ove_dir_t *d, const char *path)
{
	return ove_fs_opendir(d, path);
}

static int fs_closedir(ove_dir_t d)
{
	return ove_fs_closedir(d);
}

/*
 * Per-test sandbox created via mkdtemp so parallel/aborted runs don't
 * collide and failed tests don't leave junk in /tmp. Root is overridable
 * via OVE_TEST_TMPDIR for CI runners that need a private scratch area.
 */
static char s_tmpdir[256];
static char s_tmppath[320];

static void fs_rm_rf(const char *dir)
{
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			char p[512];
			snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
			struct stat st;
			if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
				fs_rm_rf(p);
			else
				unlink(p);
		}
		closedir(d);
	}
	rmdir(dir);
}

static int fs_setup(void **state)
{
	(void)state;
	const char *root = getenv("OVE_TEST_TMPDIR");
	if (root == NULL || *root == '\0')
		root = "/tmp";
	snprintf(s_tmpdir, sizeof(s_tmpdir), "%s/ove_test_XXXXXX", root);
	if (mkdtemp(s_tmpdir) == NULL)
		return -1;
	snprintf(s_tmppath, sizeof(s_tmppath), "%s/file", s_tmpdir);
	int fd = creat(s_tmppath, 0600);
	if (fd >= 0)
		close(fd);
	return 0;
}

static int fs_teardown(void **state)
{
	(void)state;
	fs_rm_rf(s_tmpdir);
	return 0;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_fs_mount(void **state)
{
	(void)state;
	int rc = ove_fs_mount(NULL, "/");
	assert_int_equal(rc, OVE_OK);
	ove_fs_unmount("/");
}

static void test_fs_open_close(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");

	ove_file_t f = NULL;
	int rc = fs_open(&f, s_tmppath, OVE_FS_O_READ | OVE_FS_O_WRITE);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(f);

	rc = fs_close(f);
	assert_int_equal(rc, OVE_OK);

	ove_fs_unmount("/");
}

static void test_fs_write_read(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");

	ove_file_t f = NULL;
	fs_open(&f, s_tmppath, OVE_FS_O_READ | OVE_FS_O_WRITE | OVE_FS_O_CREATE);

	const char *data = "Hello, filesystem!";
	size_t written = 0;
	int rc = ove_fs_write(f, data, strlen(data), &written);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(written, strlen(data));

	/* Seek back to start */
	ove_fs_seek(f, 0, OVE_FS_SEEK_SET);

	char buf[64] = {0};
	size_t read_n = 0;
	rc = ove_fs_read(f, buf, sizeof(buf), &read_n);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(read_n, strlen(data));
	assert_string_equal(buf, "Hello, filesystem!");

	fs_close(f);
	ove_fs_unmount("/");
}

static void test_fs_seek_tell(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");

	ove_file_t f = NULL;
	fs_open(&f, s_tmppath, OVE_FS_O_READ | OVE_FS_O_WRITE | OVE_FS_O_CREATE);

	const char *data = "ABCDEFGH";
	size_t written = 0;
	ove_fs_write(f, data, 8, &written);

	ove_fs_seek(f, 4, OVE_FS_SEEK_SET);

	long pos = ove_fs_tell(f);
	assert_int_equal(pos, 4);

	char buf[4] = {0};
	size_t read_n = 0;
	ove_fs_read(f, buf, 4, &read_n);
	assert_memory_equal(buf, "EFGH", 4);

	fs_close(f);
	ove_fs_unmount("/");
}

static void test_fs_file_size(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");

	ove_file_t f = NULL;
	fs_open(&f, s_tmppath, OVE_FS_O_READ | OVE_FS_O_WRITE | OVE_FS_O_CREATE);

	const char *data = "1234567890";
	size_t written = 0;
	ove_fs_write(f, data, 10, &written);

	size_t sz = 0;
	int rc = ove_fs_size(f, &sz);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(sz, 10);

	fs_close(f);
	ove_fs_unmount("/");
}

static void test_fs_opendir_readdir_closedir(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");

	ove_dir_t d = NULL;
	int rc = fs_opendir(&d, s_tmpdir);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(d);

	struct ove_dirent entry;
	/* Sandbox contains exactly one file ("file") created by fs_setup. */
	rc = ove_fs_readdir(d, &entry);
	assert_int_equal(rc, OVE_OK);
	do {
		rc = ove_fs_readdir(d, &entry);
	} while (rc == OVE_OK);
	assert_int_equal(rc, OVE_ERR_EOF);

	rc = fs_closedir(d);
	assert_int_equal(rc, OVE_OK);

	ove_fs_unmount("/");
}

static void test_fs_unlink(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");

	/* Create a file to unlink */
	char unlinkpath[128];
	snprintf(unlinkpath, sizeof(unlinkpath), "/tmp/ove_unlink_XXXXXX");
	int fd = mkstemp(unlinkpath);
	if (fd >= 0)
		close(fd);

	int rc = ove_fs_unlink(unlinkpath);
	assert_int_equal(rc, OVE_OK);

	/* Open should fail now */
	ove_file_t f = NULL;
	rc = fs_open(&f, unlinkpath, OVE_FS_O_READ);
	assert_int_not_equal(rc, OVE_OK);

	ove_fs_unmount("/");
}

static void test_fs_rename(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");

	char srcpath[128];
	snprintf(srcpath, sizeof(srcpath), "/tmp/ove_rename_src_XXXXXX");
	int fd = mkstemp(srcpath);
	if (fd >= 0)
		close(fd);

	char dstpath[128];
	snprintf(dstpath, sizeof(dstpath), "/tmp/ove_rename_dst_%d", (int)getpid());

	int rc = ove_fs_rename(srcpath, dstpath);
	assert_int_equal(rc, OVE_OK);

	/* Clean up destination */
	unlink(dstpath);

	ove_fs_unmount("/");
}

static void test_fs_open_nonexistent(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");

	ove_file_t f = NULL;
	int rc = fs_open(&f, "/tmp/ove_nonexistent_file_xyz", OVE_FS_O_READ);
	assert_int_equal(rc, OVE_ERR_NOT_FOUND);

	ove_fs_unmount("/");
}

static void test_fs_open_creation_semantics(void **state)
{
	(void)state;
	ove_file_t f = NULL;
	const char payload[] = "preserve";

	assert_int_equal(fs_open(&f, s_tmppath, OVE_FS_O_WRITE), OVE_OK);
	assert_int_equal(ove_fs_write(f, payload, sizeof(payload) - 1, NULL), OVE_OK);
	assert_int_equal(fs_close(f), OVE_OK);

	/* CREATE must not imply truncation. */
	assert_int_equal(fs_open(&f, s_tmppath, OVE_FS_O_READ | OVE_FS_O_CREATE), OVE_OK);
	size_t size = 0;
	assert_int_equal(ove_fs_size(f, &size), OVE_OK);
	assert_int_equal(size, sizeof(payload) - 1);
	assert_int_equal(fs_close(f), OVE_OK);

	assert_int_equal(fs_open(&f, s_tmppath, OVE_FS_O_WRITE | OVE_FS_O_CREATE | OVE_FS_O_EXCL),
			 OVE_ERR_ALREADY_EXISTS);

	assert_int_equal(fs_open(&f, s_tmppath, OVE_FS_O_WRITE | OVE_FS_O_TRUNC), OVE_OK);
	assert_int_equal(ove_fs_size(f, &size), OVE_OK);
	assert_int_equal(size, 0);
	assert_int_equal(fs_close(f), OVE_OK);
}

static void test_fs_append_truncate_sync(void **state)
{
	(void)state;
	ove_file_t f = NULL;

	assert_int_equal(fs_open(&f, s_tmppath, OVE_FS_O_READ | OVE_FS_O_WRITE | OVE_FS_O_TRUNC),
			 OVE_OK);
	assert_int_equal(ove_fs_write(f, "abc", 3, NULL), OVE_OK);
	assert_int_equal(fs_close(f), OVE_OK);

	assert_int_equal(fs_open(&f, s_tmppath, OVE_FS_O_READ | OVE_FS_O_WRITE | OVE_FS_O_APPEND),
			 OVE_OK);
	assert_int_equal(ove_fs_seek(f, 0, OVE_FS_SEEK_SET), OVE_OK);
	assert_int_equal(ove_fs_write(f, "d", 1, NULL), OVE_OK);
	assert_int_equal(ove_fs_truncate(f, 2), OVE_OK);
	assert_int_equal(ove_fs_sync(f), OVE_OK);
	size_t size = 0;
	assert_int_equal(ove_fs_size(f, &size), OVE_OK);
	assert_int_equal(size, 2);
	assert_int_equal(fs_close(f), OVE_OK);
}

static void test_fs_directory_and_stat(void **state)
{
	(void)state;
	char dirpath[320];
	char childpath[352];
	snprintf(dirpath, sizeof(dirpath), "%s/subdir", s_tmpdir);
	snprintf(childpath, sizeof(childpath), "%s/child", dirpath);

	assert_int_equal(ove_fs_mkdir(dirpath), OVE_OK);
	assert_int_equal(ove_fs_mkdir(dirpath), OVE_ERR_ALREADY_EXISTS);

	struct ove_fs_stat st;
	assert_int_equal(ove_fs_stat(dirpath, &st), OVE_OK);
	assert_int_equal(st.type, OVE_FS_TYPE_DIR);

	ove_file_t f = NULL;
	assert_int_equal(fs_open(&f, childpath, OVE_FS_O_WRITE | OVE_FS_O_CREATE), OVE_OK);
	assert_int_equal(ove_fs_write(f, "x", 1, NULL), OVE_OK);
	assert_int_equal(fs_close(f), OVE_OK);
	assert_int_equal(ove_fs_stat(childpath, &st), OVE_OK);
	assert_int_equal(st.type, OVE_FS_TYPE_FILE);
	assert_int_equal(st.size, 1);

	assert_int_equal(ove_fs_rmdir(dirpath), OVE_ERR_NOT_EMPTY);
	assert_int_equal(ove_fs_unlink(childpath), OVE_OK);
	assert_int_equal(ove_fs_rmdir(dirpath), OVE_OK);
	assert_int_equal(ove_fs_stat(dirpath, &st), OVE_ERR_NOT_FOUND);
}

static void test_fs_volume_stat(void **state)
{
	(void)state;
	struct ove_fs_statvfs stat;

	assert_int_equal(ove_fs_mount(NULL, "/"), OVE_OK);
	assert_int_equal(ove_fs_statvfs(&stat), OVE_OK);
	assert_true(stat.block_size > 0u);
	assert_true(stat.fragment_size > 0u);
	assert_true(stat.blocks > 0u);
	assert_true(stat.blocks_free <= stat.blocks);
	assert_true(stat.blocks_available <= stat.blocks_free);
	assert_true(stat.name_max > 0u);
	ove_fs_unmount("/");
}

static void test_fs_unmount(void **state)
{
	(void)state;
	ove_fs_mount(NULL, "/");
	/* Should not crash */
	ove_fs_unmount("/");
}

/* ── runner ──────────────────────────────────────────────────────────── */

#endif /* !CONFIG_OVE_ZERO_HEAP */

int test_fs_run(void)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	printf("  [SKIP] fs tests (POSIX backend, no static storage in sim zeroheap)\n");
	return 0;
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(test_fs_mount, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_volume_stat, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_open_close, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_write_read, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_seek_tell, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_file_size, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_opendir_readdir_closedir, fs_setup,
						fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_unlink, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_rename, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_open_nonexistent, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_open_creation_semantics, fs_setup,
						fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_append_truncate_sync, fs_setup,
						fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_directory_and_stat, fs_setup, fs_teardown),
		cmocka_unit_test_setup_teardown(test_fs_unmount, fs_setup, fs_teardown),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
#endif /* !CONFIG_OVE_ZERO_HEAP */
}
