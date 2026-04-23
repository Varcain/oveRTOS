#include "../framework/ove_test.hpp"

struct pair_t {
	int a;
	int b;
};

static std::atomic<int> s_cpp_consumer_sum;
static std::atomic<int> s_cpp_blocking_received;

extern "C" {

static void cpp_consumer_thread(void *arg)
{
	auto *q = static_cast<ove::Queue<int, 10> *>(arg);
	int val;
	while (q->receive(&val, 200) == OVE_OK)
		s_cpp_consumer_sum.fetch_add(val);
}

static void cpp_blocking_receiver(void *arg)
{
	auto *q = static_cast<ove::Queue<int, 10> *>(arg);
	int val;
	if (q->receive(&val, OVE_WAIT_FOREVER) == OVE_OK)
		s_cpp_blocking_received.store(val);
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_queue_create_destroy(void **state)
{
	(void)state;
	ove::Queue<int, 5> q;
	assert_true(q.valid());
}

static void test_cpp_queue_send_receive_single(void **state)
{
	(void)state;
	ove::Queue<int, 5> q;

	int send_val = 42;
	assert_int_equal(q.send(send_val, 0), OVE_OK);

	int recv_val = 0;
	assert_int_equal(q.receive(&recv_val, 0), OVE_OK);
	assert_int_equal(recv_val, 42);
}

static void test_cpp_queue_fifo_order(void **state)
{
	(void)state;
	ove::Queue<int, 10> q;

	for (int i = 0; i < 5; i++)
		(void)q.send(i, 0);

	for (int i = 0; i < 5; i++) {
		int val = -1;
		assert_int_equal(q.receive(&val, 0), OVE_OK);
		assert_int_equal(val, i);
	}
}

static void test_cpp_queue_send_full_times_out(void **state)
{
	(void)state;
	ove::Queue<int, 2> q;

	int v = 1;
	assert_int_equal(q.send(v, 0), OVE_OK);
	v = 2;
	assert_int_equal(q.send(v, 0), OVE_OK);
	v = 3;
	assert_int_equal(q.send(v, 10), OVE_ERR_TIMEOUT);
}

static void test_cpp_queue_receive_empty_times_out(void **state)
{
	(void)state;
	ove::Queue<int, 5> q;

	int val;
	assert_int_equal(q.receive(&val, 10), OVE_ERR_TIMEOUT);
}

static void test_cpp_queue_send_from_isr(void **state)
{
	(void)state;
	ove::Queue<int, 5> q;

	int v = 99;
	(void)q.send_from_isr(v);

	int out = 0;
	assert_int_equal(q.receive(&out, 0), OVE_OK);
	assert_int_equal(out, 99);
}

static void test_cpp_queue_receive_from_isr(void **state)
{
	(void)state;
	ove::Queue<int, 5> q;

	int v = 77;
	(void)q.send(v, 0);

	int out = 0;
	assert_int_equal(q.receive_from_isr(&out), OVE_OK);
	assert_int_equal(out, 77);
}

static void test_cpp_queue_producer_consumer(void **state)
{
	(void)state;
	ove::Queue<int, 10> q;

	s_cpp_consumer_sum.store(0);

	{
		auto th = make_test_thread("consumer", cpp_consumer_thread, &q, OVE_PRIO_LOW);

		for (int i = 1; i <= 5; i++) {
			(void)q.send(i, 100);
			test_msleep(5);
		}

		test_msleep(500);
	}

	assert_int_equal(s_cpp_consumer_sum.load(), 15);
}

static void test_cpp_queue_struct_item(void **state)
{
	(void)state;
	ove::Queue<pair_t, 4> q;

	pair_t p = {10, 20};
	assert_int_equal(q.send(p, 0), OVE_OK);

	pair_t out = {};
	assert_int_equal(q.receive(&out, 0), OVE_OK);
	assert_int_equal(out.a, 10);
	assert_int_equal(out.b, 20);
}

static void test_cpp_queue_send_wait_forever(void **state)
{
	(void)state;
	ove::Queue<int, 10> q;

	s_cpp_blocking_received.store(0);

	auto th = make_test_thread("blocker", cpp_blocking_receiver, &q, OVE_PRIO_LOW);

	test_msleep(50);

	int v = 123;
	(void)q.send(v, 0);

	test_msleep(100);
	assert_int_equal(s_cpp_blocking_received.load(), 123);
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_queue_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Queue<int, 5> q;
		int v = 1;
		(void)q.send(v, 0);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_queue_move_construct(void **state)
{
	(void)state;
	ove::Queue<int, 5> a;
	assert_true(a.valid());

	ove::Queue<int, 5> b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_queue_type_safety(void **state)
{
	(void)state;
	/* Queue<uint8_t, N> stores 1-byte items, Queue<uint32_t, N> stores 4-byte */
	ove::Queue<uint8_t, 4> q8;
	uint8_t v8 = 0xAB;
	assert_int_equal(q8.send(v8, 0), OVE_OK);
	uint8_t out8 = 0;
	assert_int_equal(q8.receive(&out8, 0), OVE_OK);
	assert_int_equal(out8, 0xAB);

	ove::Queue<uint32_t, 4> q32;
	uint32_t v32 = 0xDEADBEEF;
	assert_int_equal(q32.send(v32, 0), OVE_OK);
	uint32_t out32 = 0;
	assert_int_equal(q32.receive(&out32, 0), OVE_OK);
	assert_true(out32 == 0xDEADBEEF);
}

static void test_cpp_queue_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Queue<int, 5>>::value,
		      "Queue must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Queue<int, 5>>::value,
		      "Queue must not be copy assignable");
}

int test_cpp_queue_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_queue_create_destroy),
		cmocka_unit_test(test_cpp_queue_send_receive_single),
		cmocka_unit_test(test_cpp_queue_fifo_order),
		cmocka_unit_test(test_cpp_queue_send_full_times_out),
		cmocka_unit_test(test_cpp_queue_receive_empty_times_out),
		cmocka_unit_test(test_cpp_queue_send_from_isr),
		cmocka_unit_test(test_cpp_queue_receive_from_isr),
		cmocka_unit_test(test_cpp_queue_producer_consumer),
		cmocka_unit_test(test_cpp_queue_struct_item),
		cmocka_unit_test(test_cpp_queue_send_wait_forever),
		cmocka_unit_test(test_cpp_queue_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_queue_move_construct),
#endif
		cmocka_unit_test(test_cpp_queue_type_safety),
		cmocka_unit_test(test_cpp_queue_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
