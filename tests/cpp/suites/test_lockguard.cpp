#include "../framework/ove_test.hpp"

struct cpp_lg_ctx {
	ove::Mutex *mtx;
	int *counter;
};

extern "C" {

static void cpp_lg_increment_entry(void *arg)
{
	auto *ctx = static_cast<cpp_lg_ctx *>(arg);
	for (int i = 0; i < 1000; i++) {
		ove::LockGuard guard(*ctx->mtx);
		(*ctx->counter)++;
	}
}

} /* extern "C" */

/* ── LockGuard tests (C++ only) ─────────────────────────────────────── */

static void test_cpp_lockguard_basic(void **state)
{
	(void)state;
	ove::Mutex mtx;

	{
		ove::LockGuard guard(mtx);
		/* Mutex should be locked here */
		assert_int_equal(mtx.lock(std::chrono::milliseconds{0}), OVE_ERR_TIMEOUT);
	}
	/* Mutex should be unlocked after guard goes out of scope */
	assert_int_equal(mtx.lock(std::chrono::milliseconds{0}), OVE_OK);
	mtx.unlock();
}

static void test_cpp_lockguard_scope_exit(void **state)
{
	(void)state;
	ove::Mutex mtx;

	/* Lock via guard, verify auto-unlock */
	{
		ove::LockGuard guard(mtx);
	}
	/* Should be unlocked now — try-lock should succeed */
	assert_int_equal(mtx.lock(std::chrono::milliseconds{0}), OVE_OK);
	mtx.unlock();
}

static void test_cpp_lockguard_counter_protection(void **state)
{
	(void)state;
	ove::Mutex mtx;
	int counter = 0;
	cpp_lg_ctx ctx = {&mtx, &counter};

	{
		auto t1 = make_test_thread("lg1", cpp_lg_increment_entry, &ctx);
		auto t2 = make_test_thread("lg2", cpp_lg_increment_entry, &ctx);
	}

	assert_int_equal(counter, 2000);
}

static void test_cpp_lockguard_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::LockGuard>::value,
		      "LockGuard must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::LockGuard>::value,
		      "LockGuard must not be copy assignable");
}

static void test_cpp_lockguard_not_movable(void **state)
{
	(void)state;
	static_assert(!std::is_move_constructible<ove::LockGuard>::value,
		      "LockGuard must not be move constructible");
	static_assert(!std::is_move_assignable<ove::LockGuard>::value,
		      "LockGuard must not be move assignable");
}

int test_cpp_lockguard_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_lockguard_basic),
		cmocka_unit_test(test_cpp_lockguard_scope_exit),
		cmocka_unit_test(test_cpp_lockguard_counter_protection),
		cmocka_unit_test(test_cpp_lockguard_not_copyable),
		cmocka_unit_test(test_cpp_lockguard_not_movable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
