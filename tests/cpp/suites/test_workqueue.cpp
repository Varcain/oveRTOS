#include "../framework/ove_test.hpp"

static std::atomic<int> s_wq_count;

static void test_wq_handler(ove_work_t work)
{
	(void)work;
	s_wq_count++;
}

static void test_cpp_workqueue_create_destroy(void **state)
{
	(void)state;
	ove::Workqueue<4096> wq("test_wq", OVE_PRIO_NORMAL);
	assert_true(wq.valid());
}

static void test_cpp_work_init_free(void **state)
{
	(void)state;
	ove::Work w(test_wq_handler);
	assert_true(w.valid());
}

static void test_cpp_work_submit(void **state)
{
	(void)state;
	s_wq_count = 0;

	ove::Workqueue<4096> wq("sub_wq", OVE_PRIO_NORMAL);

	ove::Work w(test_wq_handler);

	OVE_TEST_ASSERT_OK(w.submit(wq));
	test_msleep(100);
	assert_int_equal(s_wq_count, 1);
}

static void test_cpp_work_submit_delayed(void **state)
{
	(void)state;
	s_wq_count = 0;

	ove::Workqueue<4096> wq("del_wq", OVE_PRIO_NORMAL);

	ove::Work w(test_wq_handler);

	OVE_TEST_ASSERT_OK(w.submit_delayed(wq, 50));

	/* Should not have fired immediately */
	test_msleep(10);
	assert_int_equal(s_wq_count, 0);

	test_msleep(150);
	assert_int_equal(s_wq_count, 1);
}

static void test_cpp_work_cancel(void **state)
{
	(void)state;
	ove::Workqueue<4096> wq("can_wq", OVE_PRIO_NORMAL);

	ove::Work w(test_wq_handler);

	/* Cancel is best-effort */
	(void)w.cancel();
}

static void test_cpp_workqueue_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Workqueue<4096> wq("raii_wq", OVE_PRIO_NORMAL);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_workqueue_move_construct(void **state)
{
	(void)state;
	ove::Workqueue<4096> a("mv_wq", OVE_PRIO_NORMAL);
	assert_true(a.valid());

	ove::Workqueue<4096> b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_workqueue_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Workqueue<4096>>::value,
		      "Workqueue must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Workqueue<4096>>::value,
		      "Workqueue must not be copy assignable");
	static_assert(!std::is_copy_constructible<ove::Work>::value,
		      "Work must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Work>::value,
		      "Work must not be copy assignable");
}

int test_cpp_workqueue_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_workqueue_create_destroy),
		cmocka_unit_test(test_cpp_work_init_free),
		cmocka_unit_test(test_cpp_work_submit),
		cmocka_unit_test(test_cpp_work_submit_delayed),
		cmocka_unit_test(test_cpp_work_cancel),
		cmocka_unit_test(test_cpp_workqueue_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_workqueue_move_construct),
#endif
		cmocka_unit_test(test_cpp_workqueue_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
