#include "../framework/ove_test.hpp"

#include <vector>

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
	while (q->try_receive_for(val, std::chrono::milliseconds{200}).has_value())
		s_cpp_consumer_sum.fetch_add(val);
}

static void cpp_blocking_receiver(void *arg)
{
	auto *q = static_cast<ove::Queue<int, 10> *>(arg);
	int val;
	q->receive(val);
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
	assert_true(q.try_send(send_val));

	int recv_val = 0;
	assert_true(q.try_receive(recv_val));
	assert_int_equal(recv_val, 42);
}

static void test_cpp_queue_fifo_order(void **state)
{
	(void)state;
	ove::Queue<int, 10> q;

	for (int i = 0; i < 5; i++)
		(void)q.try_send(i);

	for (int i = 0; i < 5; i++) {
		int val = -1;
		assert_true(q.try_receive(val));
		assert_int_equal(val, i);
	}
}

static void test_cpp_queue_send_full_times_out(void **state)
{
	(void)state;
	ove::Queue<int, 2> q;

	int v = 1;
	assert_true(q.try_send(v));
	v = 2;
	assert_true(q.try_send(v));
	v = 3;
	ove::Result<void> r = q.try_send_for(v, std::chrono::milliseconds{10});
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout || r.error() == ove::Error::QueueFull);
}

static void test_cpp_queue_receive_empty_times_out(void **state)
{
	(void)state;
	ove::Queue<int, 5> q;

	int val;
	ove::Result<void> r = q.try_receive_for(val, std::chrono::milliseconds{10});
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout || r.error() == ove::Error::QueueEmpty);
}

static void test_cpp_queue_send_from_isr(void **state)
{
	(void)state;
	ove::Queue<int, 5> q;

	int v = 99;
	(void)q.send_from_isr(v);

	int out = 0;
	assert_true(q.try_receive(out));
	assert_int_equal(out, 99);
}

static void test_cpp_queue_receive_from_isr(void **state)
{
	(void)state;
	ove::Queue<int, 5> q;

	int v = 77;
	(void)q.try_send(v);

	int out = 0;
	assert_true(q.receive_from_isr(out).has_value());
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
			(void)q.try_send_for(i, std::chrono::milliseconds{100});
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
	assert_true(q.try_send(p));

	pair_t out = {};
	assert_true(q.try_receive(out));
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
	(void)q.try_send(v);

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
		(void)q.try_send(v);
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
	assert_true(q8.try_send(v8));
	uint8_t out8 = 0;
	assert_true(q8.try_receive(out8));
	assert_int_equal(out8, 0xAB);

	ove::Queue<uint32_t, 4> q32;
	uint32_t v32 = 0xDEADBEEF;
	assert_true(q32.try_send(v32));
	uint32_t out32 = 0;
	assert_true(q32.try_receive(out32));
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

/* ── Iter A1.2: trivially_copyable static_assert constraint ──────────
 *
 * The substrate memcpy()s items of sizeof(T) bytes; non-trivial types
 * (std::string, std::vector, std::unique_ptr, user types with explicit
 * copy/move ctors or destructors that do bookkeeping) corrupt their
 * internal state on memcpy.
 *
 * The Queue<T,N> template now carries a static_assert rejecting any T
 * that isn't trivially copyable.  This is a hard compile error at
 * template instantiation — there's no SFINAE-friendly way to "test"
 * that a bad type would fail to compile without itself failing the
 * build, so the verification here is structural:
 *   1. Trivially-copyable types compile (int, struct of PODs).
 *   2. Non-trivial type categories are recognized by std::is_trivially_copyable.
 *
 * Bad cases (uncomment any to manually verify the static_assert fires):
 *   // ove::Queue<std::string, 8> bad1;
 *   // ove::Queue<std::vector<int>, 8> bad2;
 *   // ove::Queue<std::unique_ptr<int>, 8> bad3;
 */
struct CppQueuePodMsg {
	int id;
	float value;
	char tag[8];
};

static void test_cpp_queue_trivially_copyable_constraint(void **state)
{
	(void)state;
	/* Positive: instantiating with trivially-copyable T compiles. */
	static_assert(std::is_trivially_copyable_v<int>);
	static_assert(std::is_trivially_copyable_v<CppQueuePodMsg>);
	static_assert(std::is_trivially_copyable_v<ove_thread_state_t>);

	/* The Queue template is constrained — declaring these aliases
	 * proves the static_assert accepts them at instantiation. */
	using QInt = ove::Queue<int, 4>;
	using QPod = ove::Queue<CppQueuePodMsg, 4>;
	using QEnum = ove::Queue<ove_thread_state_t, 4>;
	(void)sizeof(QInt);
	(void)sizeof(QPod);
	(void)sizeof(QEnum);

	/* Sanity-check the std trait recognises a non-trivial type — if
	 * this fails the static_assert in queue.hpp is the wrong check. */
	static_assert(!std::is_trivially_copyable_v<std::vector<int>>,
		      "vector should not be trivially copyable — sanity check");
}

/* Method-return-type pins.  Catches an accidental revert of the
 * `try_send_for/until` / `try_receive_for/until` migration from `int`
 * to `Result<void>` at compile time.  Forever (`send`/`receive`) and
 * immediate (`try_send`/`try_receive`) forms keep their existing
 * shapes. */
static void test_cpp_queue_try_receive_value(void **state)
{
	(void)state;
	ove::Queue<int, 4> q;

	// Empty queue -> unexpected(QueueEmpty).
	ove::Result<int> empty = q.try_receive();
	assert_false(empty.has_value());
	assert_true(empty.error() == ove::Error::QueueEmpty);

	// Item present -> value by return.
	int v = 55;
	assert_true(q.try_send(v));
	ove::Result<int> got = q.try_receive();
	assert_true(got.has_value());
	assert_int_equal(*got, 55);

	// Drained again -> unexpected(QueueEmpty).
	assert_false(q.try_receive().has_value());
}

static void test_cpp_queue_return_type_shape(void **state)
{
	(void)state;
	using Q = ove::Queue<int, 4>;
	static_assert(
		std::is_same_v<decltype(std::declval<Q>().send(std::declval<int &>())), void>);
	static_assert(
		std::is_same_v<decltype(std::declval<Q>().try_send(std::declval<int &>())), bool>);
	static_assert(std::is_same_v<decltype(std::declval<Q>().try_send_for(
					     std::declval<int &>(), std::chrono::milliseconds{1})),
				     ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<Q>().try_send_until(
				       std::declval<int &>(), std::chrono::steady_clock::now())),
			       ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<Q>().receive(std::declval<int &>())), void>);
	static_assert(std::is_same_v<decltype(std::declval<Q>().try_receive(std::declval<int &>())),
				     bool>);
	static_assert(std::is_same_v<decltype(std::declval<Q>().try_receive()), ove::Result<int>>);
	static_assert(std::is_same_v<decltype(std::declval<Q>().try_receive_for(
					     std::declval<int &>(), std::chrono::milliseconds{1})),
				     ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<Q>().try_receive_until(
				       std::declval<int &>(), std::chrono::steady_clock::now())),
			       ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<Q>().send_from_isr(std::declval<int &>())),
			       ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<Q>().receive_from_isr(std::declval<int &>())),
			       ove::Result<void>>);
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
		cmocka_unit_test(test_cpp_queue_trivially_copyable_constraint),
		cmocka_unit_test(test_cpp_queue_try_receive_value),
		cmocka_unit_test(test_cpp_queue_return_type_shape),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
