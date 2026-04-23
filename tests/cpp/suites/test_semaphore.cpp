#include "../framework/ove_test.hpp"

struct cpp_sem_ctx {
	ove::Semaphore *sem;
	volatile int done;
};

extern "C" {

static void cpp_sem_give_entry(void *arg)
{
	auto *ctx = static_cast<cpp_sem_ctx *>(arg);
	test_msleep(50);
	ctx->sem->give();
	ctx->done = 1;
}

static void cpp_sem_give_delayed_entry(void *arg)
{
	auto *ctx = static_cast<cpp_sem_ctx *>(arg);
	test_msleep(100);
	ctx->sem->give();
	ctx->done = 1;
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_sem_create_binary(void **state)
{
	(void)state;
	ove::Semaphore sem(1, 1);
	assert_true(sem.valid());
}

static void test_cpp_sem_create_counting(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 10);
}

static void test_cpp_sem_take_initial_one(void **state)
{
	(void)state;
	ove::Semaphore sem(1, 1);
	assert_int_equal(sem.take(0), OVE_OK);
}

static void test_cpp_sem_take_timeout(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 10);
	assert_int_equal(sem.take(50), OVE_ERR_TIMEOUT);
}

static void test_cpp_sem_give_then_take(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 10);
	sem.give();
	assert_int_equal(sem.take(0), OVE_OK);
}

static void test_cpp_sem_counting(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 10);
	for (int i = 0; i < 3; i++)
		sem.give();
	for (int i = 0; i < 3; i++)
		assert_int_equal(sem.take(0), OVE_OK);
	assert_int_equal(sem.take(10), OVE_ERR_TIMEOUT);
}

static void test_cpp_sem_producer_consumer(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 1);
	cpp_sem_ctx ctx = {&sem, 0};

	{
		auto th = make_test_thread("prod", cpp_sem_give_entry, &ctx);
		assert_int_equal(sem.take(500), OVE_OK);
	}
	assert_int_equal(ctx.done, 1);
}

static void test_cpp_sem_wait_forever(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 1);
	cpp_sem_ctx ctx = {&sem, 0};

	auto th = make_test_thread("wf", cpp_sem_give_delayed_entry, &ctx);
	assert_int_equal(sem.take(OVE_WAIT_FOREVER), OVE_OK);
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_sem_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Semaphore sem(1, 1);
		(void)sem.take(0);
		sem.give();
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_sem_move_construct(void **state)
{
	(void)state;
	ove::Semaphore a(1, 1);
	assert_true(a.valid());

	ove::Semaphore b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_sem_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Semaphore>::value,
		      "Semaphore must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Semaphore>::value,
		      "Semaphore must not be copy assignable");
}

int test_cpp_semaphore_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_sem_create_binary),
		cmocka_unit_test(test_cpp_sem_create_counting),
		cmocka_unit_test(test_cpp_sem_take_initial_one),
		cmocka_unit_test(test_cpp_sem_take_timeout),
		cmocka_unit_test(test_cpp_sem_give_then_take),
		cmocka_unit_test(test_cpp_sem_counting),
		cmocka_unit_test(test_cpp_sem_producer_consumer),
		cmocka_unit_test(test_cpp_sem_wait_forever),
		cmocka_unit_test(test_cpp_sem_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_sem_move_construct),
#endif
		cmocka_unit_test(test_cpp_sem_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
