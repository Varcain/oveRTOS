#include "../framework/ove_test.hpp"

#include <mutex>

struct cpp_counter_ctx {
	ove::Mutex *mtx;
	int counter;
};

extern "C" {

static void cpp_counter_entry(void *arg)
{
	auto *ctx = static_cast<cpp_counter_ctx *>(arg);
	for (int i = 0; i < 1000; i++) {
		(void)ctx->mtx->lock();
		ctx->counter++;
		ctx->mtx->unlock();
	}
}

struct cpp_hold_ctx {
	ove::Mutex *mtx;
	volatile int locked;
	volatile int released;
	int hold_ms;
};

static void cpp_hold_entry(void *arg)
{
	auto *ctx = static_cast<cpp_hold_ctx *>(arg);
	(void)ctx->mtx->lock();
	__atomic_store_n(&ctx->locked, 1, __ATOMIC_RELEASE);
	test_msleep(ctx->hold_ms);
	ctx->mtx->unlock();
	__atomic_store_n(&ctx->released, 1, __ATOMIC_RELEASE);
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_mutex_create(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_true(mtx.valid());
}

static void test_cpp_mutex_destroy_safe(void **state)
{
	(void)state;
	ove::Mutex mtx;
	/* RAII: destructor cleans up */
}

static void test_cpp_mutex_lock_unlock(void **state)
{
	(void)state;
	ove::Mutex mtx;
	mtx.lock();
	mtx.unlock();
}

static void test_cpp_mutex_contention_timeout(void **state)
{
	(void)state;
	ove::Mutex mtx;

	cpp_hold_ctx ctx = {&mtx, 0, 0, 200};
	auto th = make_test_thread("hold", cpp_hold_entry, &ctx);
	for (int i = 0; i < 500 && !__atomic_load_n(&ctx.locked, __ATOMIC_ACQUIRE); i++)
		test_msleep(1);

	ove::Result<void> r = mtx.try_lock_for(std::chrono::milliseconds{50});
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout);
}

static void test_cpp_mutex_contention_success(void **state)
{
	(void)state;
	ove::Mutex mtx;

	cpp_hold_ctx ctx = {&mtx, 0, 0, 50};
	auto th = make_test_thread("rel", cpp_hold_entry, &ctx);
	for (int i = 0; i < 500 && !__atomic_load_n(&ctx.locked, __ATOMIC_ACQUIRE); i++)
		test_msleep(1);

	ove::Result<void> r = mtx.try_lock_for(std::chrono::milliseconds{500});
	assert_true(r.has_value());
	mtx.unlock();
}

static void test_cpp_mutex_double_unlock(void **state)
{
	(void)state;
#ifdef __SANITIZE_THREAD__
	skip(); /* TSan flags double-unlock as UB; same rationale as C side. */
#else
	ove::Mutex mtx;
	mtx.lock();
	mtx.unlock();
	mtx.unlock(); /* should not crash */
#endif
}

static void test_cpp_mutex_zero_timeout_free(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_true(mtx.try_lock());
	mtx.unlock();
}

static void test_cpp_mutex_shared_counter(void **state)
{
	(void)state;
	ove::Mutex mtx;

	cpp_counter_ctx ctx = {&mtx, 0};
	{
		auto t1 = make_test_thread("c1", cpp_counter_entry, &ctx);
		auto t2 = make_test_thread("c2", cpp_counter_entry, &ctx);
	}

	assert_int_equal(ctx.counter, 2000);
}

static void test_cpp_mutex_short_timeout(void **state)
{
	(void)state;
	ove::Mutex mtx;

	cpp_hold_ctx ctx = {&mtx, 0, 0, 200};
	auto th = make_test_thread("h2", cpp_hold_entry, &ctx);
	for (int i = 0; i < 500 && !__atomic_load_n(&ctx.locked, __ATOMIC_ACQUIRE); i++)
		test_msleep(1);

	uint64_t start = 0, end = 0;
	ove_time_get_us(&start);
	ove::Result<void> r = mtx.try_lock_for(std::chrono::milliseconds{50});
	ove_time_get_us(&end);

	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout);
	assert_duration_within(end - start, 50, OVE_TEST_TIMING_TOLERANCE_MS);
}

static void test_cpp_mutex_multiple_independent(void **state)
{
	(void)state;
	ove::Mutex a, b;
	a.lock();
	b.lock();
	b.unlock();
	a.unlock();
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_mutex_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Mutex mtx;
		mtx.lock();
		mtx.unlock();
		/* mtx goes out of scope — destructor cleans up */
	}
	/* If we get here without crash, RAII destroy works */
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move semantics are only supported in heap-allocating mode: in zero-heap
 * builds the wrapper owns inline storage and Move is deleted to prevent
 * dangling-handle bugs. */
/* Verify a moved-to mutex actually works: the handle transferred intact, so it
 * locks, self-fails a non-blocking try_lock while held, and re-locks once
 * released.  A bare `.valid()` flag check would pass even if the handle were
 * lost. */
static void assert_mutex_usable(ove::Mutex &m)
{
	m.lock();
	assert_false(m.try_lock()); /* held -> non-recursive try_lock fails */
	m.unlock();
	assert_true(m.try_lock()); /* now free */
	m.unlock();
}

static void test_cpp_mutex_move_construct(void **state)
{
	(void)state;
	ove::Mutex a;
	assert_true(a.valid());

	ove::Mutex b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());

	assert_mutex_usable(b);
	/* `a` is now handle-less; its destructor at scope end must NOT double-free
	 * the handle `b` owns — caught by cpp-sanitize (ASan). */
}

static void test_cpp_mutex_move_assign(void **state)
{
	(void)state;
	ove::Mutex a, b;

	b = std::move(a);
	assert_true(b.valid());
	assert_false(a.valid());

	assert_mutex_usable(b);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_mutex_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Mutex>::value,
		      "Mutex must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Mutex>::value,
		      "Mutex must not be copy assignable");
}

/* Method-return-type pins.  Catches an accidental revert of the
 * `try_lock_for`/`try_lock_until` migration from `int` to
 * `Result<void>` at compile time. */
static void test_cpp_mutex_return_type_shape(void **state)
{
	(void)state;
	static_assert(std::is_same_v<decltype(std::declval<ove::Mutex>().lock()), void>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Mutex>().try_lock()), bool>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Mutex>().try_lock_for(
					     std::chrono::milliseconds{1})),
				     ove::Result<void>>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Mutex>().try_lock_until(
					     std::chrono::steady_clock::now())),
				     ove::Result<void>>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Mutex>().unlock()), void>);
}

static void test_cpp_mutex_valid_after_construct(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_true(mtx.valid());
}

static void test_cpp_mutex_handle_access(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_true(mtx.handle() != nullptr);
}

/* ── Iter A2.1: std::Lockable composition ────────────────────────────
 *
 * ove::Mutex now satisfies std::Lockable (lock/try_lock/unlock).
 * std::lock_guard, std::scoped_lock, std::unique_lock, and
 * std::condition_variable_any compose with it directly.  Two
 * concrete proofs:
 */
static void test_cpp_mutex_std_lock_guard_composition(void **state)
{
	(void)state;
	ove::Mutex mtx;
	{
		std::lock_guard<ove::Mutex> g{mtx};
		/* In scope: mtx is held.  Try a non-blocking try_lock — must
		 * fail (would self-deadlock on a non-recursive mutex). */
		assert_false(mtx.try_lock());
	}
	/* Out of scope: mtx is released.  Another try_lock succeeds. */
	assert_true(mtx.try_lock());
	mtx.unlock();
}

static void test_cpp_mutex_std_scoped_lock_composition(void **state)
{
	(void)state;
	ove::Mutex a, b;
	{
		std::scoped_lock<ove::Mutex, ove::Mutex> g{a, b};
		/* Both held — deadlock-free acquisition order via std::lock. */
		assert_false(a.try_lock());
		assert_false(b.try_lock());
	}
	/* Released. */
	assert_true(a.try_lock());
	assert_true(b.try_lock());
	a.unlock();
	b.unlock();
}

int test_cpp_mutex_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_mutex_create),
		cmocka_unit_test(test_cpp_mutex_destroy_safe),
		cmocka_unit_test(test_cpp_mutex_lock_unlock),
		cmocka_unit_test(test_cpp_mutex_contention_timeout),
		cmocka_unit_test(test_cpp_mutex_contention_success),
		cmocka_unit_test(test_cpp_mutex_double_unlock),
		cmocka_unit_test(test_cpp_mutex_zero_timeout_free),
		cmocka_unit_test(test_cpp_mutex_shared_counter),
		cmocka_unit_test(test_cpp_mutex_short_timeout),
		cmocka_unit_test(test_cpp_mutex_multiple_independent),
		cmocka_unit_test(test_cpp_mutex_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_mutex_move_construct),
		cmocka_unit_test(test_cpp_mutex_move_assign),
#endif
		cmocka_unit_test(test_cpp_mutex_not_copyable),
		cmocka_unit_test(test_cpp_mutex_return_type_shape),
		cmocka_unit_test(test_cpp_mutex_valid_after_construct),
		cmocka_unit_test(test_cpp_mutex_handle_access),
		cmocka_unit_test(test_cpp_mutex_std_lock_guard_composition),
		cmocka_unit_test(test_cpp_mutex_std_scoped_lock_composition),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
