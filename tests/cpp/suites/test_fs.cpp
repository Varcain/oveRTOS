#include "../framework/ove_test.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

/*
 * The C++ ove::File / ove::Dir wrappers only wrap the heap-backed
 * ove_fs_open / ove_fs_opendir APIs; they have no static-storage
 * equivalent. In zero-heap builds those backend symbols aren't
 * compiled in, so the whole suite collapses to the not_copyable
 * static_assert test.
 */
#ifndef CONFIG_OVE_ZERO_HEAP

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
	assert_true(ove::fs::mount("/dev/test", "/").has_value());
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
		assert_true(f.open(path, OVE_FS_O_CREATE | OVE_FS_O_WRITE).has_value());
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
		auto w = f.write(msg, 5);
		assert_true(w.has_value());
		assert_int_equal(*w, 5);
	}

	{
		ove::File f;
		(void)f.open(path, OVE_FS_O_READ);
		char buf[16] = {};
		auto r = f.read(buf, sizeof(buf));
		assert_true(r.has_value());
		assert_int_equal(*r, 5);
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
		(void)f.write(data, 6);

		assert_true(f.seek(0, OVE_FS_SEEK_SET).has_value());
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
		(void)f.write(data, 5);

		auto sz = f.size();
		assert_true(sz.has_value());
		assert_int_equal(*sz, 5);
	}

	ove::fs::unmount("/");
}

static void test_cpp_fs_dir_open_readdir(void **state)
{
	(void)state;
	ensure_tmpdir();
	(void)ove::fs::mount("/dev/test", "/");

	ove::Dir d;
	assert_true(d.open(s_tmpdir).has_value());
	assert_true(d.valid());

	struct ove_dirent entry;
	/* readdir may return true (got entry), false (EOF), or error. */
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

	assert_true(ove::fs::unlink(delpath).has_value());

	{
		ove::File f;
		(void)f.open(oldpath, OVE_FS_O_CREATE | OVE_FS_O_WRITE);
	}

	assert_true(ove::fs::rename(oldpath, newpath).has_value());

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

#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_fs_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::File>::value,
		      "File must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::File>::value,
		      "File must not be copy assignable");
	static_assert(!std::is_copy_constructible<ove::Dir>::value,
		      "Dir must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Dir>::value, "Dir must not be copy assignable");
}

int test_cpp_fs_run(void)
{
	const struct CMUnitTest tests[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_fs_mount_unmount),
		cmocka_unit_test(test_cpp_fs_file_open_close),
		cmocka_unit_test(test_cpp_fs_file_write_read),
		cmocka_unit_test(test_cpp_fs_file_seek_tell),
		cmocka_unit_test(test_cpp_fs_file_size),
		cmocka_unit_test(test_cpp_fs_dir_open_readdir),
		cmocka_unit_test(test_cpp_fs_unlink_rename),
		cmocka_unit_test(test_cpp_fs_file_raii_destroy),
		cmocka_unit_test(test_cpp_fs_file_move_construct),
#endif
		cmocka_unit_test(test_cpp_fs_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
