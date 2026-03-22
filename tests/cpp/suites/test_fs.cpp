#include "../framework/ove_test.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

/* Use real temp files — the posix backend's mount is a no-op,
   so paths must be valid on the host filesystem. */
static char s_tmpdir[] = "/tmp/ove_cppfs_XXXXXX";
static bool s_tmpdir_created = false;

static void ensure_tmpdir(void)
{
	if (!s_tmpdir_created) {
		/* mkdtemp modifies in-place */
		char *r = mkdtemp(s_tmpdir);
		(void)r;
		s_tmpdir_created = true;
	}
}

static void tmppath(char *buf, size_t len, const char *name)
{
	snprintf(buf, len, "%s/%s", s_tmpdir, name);
}

static void test_cpp_fs_mount_unmount(void **state)
{
	(void)state;
	int ret = ove::fs::mount("/dev/test", "/");
	assert_int_equal(ret, OVE_OK);
	ove::fs::unmount("/");
}

static void test_cpp_fs_file_open_close(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	char path[256];
	tmppath(path, sizeof(path), "test.txt");

	{
		ove::File f;
		int ret = f.open(path, OVE_FS_O_CREATE | OVE_FS_O_WRITE);
		assert_int_equal(ret, OVE_OK);
		assert_true(f.valid());
	} /* File closed via RAII before unmount */

	ove::fs::unmount("/");
}

static void test_cpp_fs_file_write_read(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	char path[256];
	tmppath(path, sizeof(path), "rw.txt");

	{
		ove::File f;
		(void)f.open(path, OVE_FS_O_CREATE | OVE_FS_O_WRITE);
		const char *msg = "hello";
		size_t written = 0;
		int ret = f.write(msg, 5, &written);
		assert_int_equal(ret, OVE_OK);
		assert_int_equal(written, 5);
	}

	{
		ove::File f;
		(void)f.open(path, OVE_FS_O_READ);
		char buf[16] = {};
		size_t rd = 0;
		int ret = f.read(buf, sizeof(buf), &rd);
		assert_int_equal(ret, OVE_OK);
		assert_int_equal(rd, 5);
		assert_memory_equal(buf, "hello", 5);
	}

	ove::fs::unmount("/");
}

static void test_cpp_fs_file_seek_tell(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	char path[256];
	tmppath(path, sizeof(path), "seek.txt");

	{
		ove::File f;
		(void)f.open(path, OVE_FS_O_CREATE | OVE_FS_O_WRITE | OVE_FS_O_READ);
		const char *data = "abcdef";
		size_t w = 0;
		(void)f.write(data, 6, &w);

		int ret = f.seek(0, OVE_FS_SEEK_SET);
		assert_int_equal(ret, OVE_OK);
		assert_int_equal(f.tell(), 0);

		(void)f.seek(3, OVE_FS_SEEK_SET);
		assert_int_equal(f.tell(), 3);
	}

	ove::fs::unmount("/");
}

static void test_cpp_fs_file_size(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	char path[256];
	tmppath(path, sizeof(path), "sz.txt");

	{
		ove::File f;
		(void)f.open(path, OVE_FS_O_CREATE | OVE_FS_O_WRITE);
		const char *data = "12345";
		size_t w = 0;
		(void)f.write(data, 5, &w);

		size_t sz = 0;
		int ret = f.size(&sz);
		assert_int_equal(ret, OVE_OK);
		assert_int_equal(sz, 5);
	}

	ove::fs::unmount("/");
}

static void test_cpp_fs_dir_open_readdir(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	ove::Dir d;
	int ret = d.open(s_tmpdir);
	assert_int_equal(ret, OVE_OK);
	assert_true(d.valid());

	struct ove_dirent entry;
	/* readdir may return OK or error if empty */
	(void)d.readdir(&entry);

	ove::fs::unmount("/");
}

static void test_cpp_fs_unlink_rename(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	char delpath[256], oldpath[256], newpath[256];
	tmppath(delpath, sizeof(delpath), "del.txt");
	tmppath(oldpath, sizeof(oldpath), "old.txt");
	tmppath(newpath, sizeof(newpath), "new.txt");

	{
		ove::File f;
		(void)f.open(delpath, OVE_FS_O_CREATE | OVE_FS_O_WRITE);
	}

	int ret = ove::fs::unlink(delpath);
	assert_int_equal(ret, OVE_OK);

	{
		ove::File f;
		(void)f.open(oldpath, OVE_FS_O_CREATE | OVE_FS_O_WRITE);
	}

	ret = ove::fs::rename(oldpath, newpath);
	assert_int_equal(ret, OVE_OK);

	ove::fs::unmount("/");
}

static void test_cpp_fs_file_raii_destroy(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	char path[256];
	tmppath(path, sizeof(path), "raii.txt");

	{
		ove::File f;
		(void)f.open(path, OVE_FS_O_CREATE | OVE_FS_O_WRITE);
	}
	ove::fs::unmount("/");
}

static void test_cpp_fs_file_move_construct(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	char path[256];
	tmppath(path, sizeof(path), "mv.txt");

	ove::File a;
	(void)a.open(path, OVE_FS_O_CREATE | OVE_FS_O_WRITE);
	assert_true(a.valid());

	ove::File b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());

	ove::fs::unmount("/");
}

static void test_cpp_fs_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::File>::value,
		      "File must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::File>::value,
		      "File must not be copy assignable");
	static_assert(!std::is_copy_constructible<ove::Dir>::value,
		      "Dir must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Dir>::value,
		      "Dir must not be copy assignable");
}

int test_cpp_fs_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_fs_mount_unmount),
		cmocka_unit_test(test_cpp_fs_file_open_close),
		cmocka_unit_test(test_cpp_fs_file_write_read),
		cmocka_unit_test(test_cpp_fs_file_seek_tell),
		cmocka_unit_test(test_cpp_fs_file_size),
		cmocka_unit_test(test_cpp_fs_dir_open_readdir),
		cmocka_unit_test(test_cpp_fs_unlink_rename),
		cmocka_unit_test(test_cpp_fs_file_raii_destroy),
		cmocka_unit_test(test_cpp_fs_file_move_construct),
		cmocka_unit_test(test_cpp_fs_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
