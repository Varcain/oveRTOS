#include "../framework/ove_test.hpp"
#include <cstring>

static void test_cpp_stream_create_destroy(void **state)
{
	(void)state;
	ove::Stream<256> s(1);
	assert_true(s.valid());
}

static void test_cpp_stream_send_receive(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	const uint8_t tx[] = {0xDE, 0xAD, 0xBE, 0xEF};
	size_t sent = 0;
	OVE_TEST_ASSERT_OK(s.try_send_for(tx, sizeof(tx), std::chrono::milliseconds{100}, sent));
	assert_int_equal(sent, sizeof(tx));

	uint8_t rx[4] = {};
	size_t received = 0;
	OVE_TEST_ASSERT_OK(
		s.try_receive_for(rx, sizeof(rx), std::chrono::milliseconds{100}, received));
	assert_int_equal(received, sizeof(tx));
	assert_memory_equal(rx, tx, sizeof(tx));
}

static void test_cpp_stream_bytes_available(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	assert_int_equal(s.bytes_available(), 0);

	const uint8_t data[] = {1, 2, 3};
	size_t sent = 0;
	(void)s.try_send_for(data, sizeof(data), std::chrono::milliseconds{100}, sent);
	assert_true(s.bytes_available() >= 3);
}

static void test_cpp_stream_reset(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	const uint8_t data[] = {1, 2, 3};
	size_t sent = 0;
	(void)s.try_send_for(data, sizeof(data), std::chrono::milliseconds{100}, sent);
	assert_true(s.bytes_available() > 0);

	OVE_TEST_ASSERT_OK(s.reset());
	assert_int_equal(s.bytes_available(), 0);
}

static void test_cpp_stream_send_from_isr(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	const uint8_t data[] = {0xAA, 0xBB};
	size_t sent = 0;
	OVE_TEST_ASSERT_OK(s.send_from_isr(data, sizeof(data), sent));
	assert_int_equal(sent, 2);
}

static void test_cpp_stream_receive_from_isr(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	const uint8_t tx[] = {0x11, 0x22};
	size_t sent = 0;
	(void)s.try_send_for(tx, sizeof(tx), std::chrono::milliseconds{100}, sent);

	uint8_t rx[2] = {};
	size_t received = 0;
	assert_true(s.receive_from_isr(rx, sizeof(rx), received).has_value());
	assert_int_equal(received, 2);
	assert_memory_equal(rx, tx, 2);
}

static void test_cpp_stream_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Stream<128> s(1);
		const uint8_t d[] = {1};
		size_t n = 0;
		(void)s.try_send_for(d, 1, std::chrono::milliseconds{0}, n);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_stream_move_construct(void **state)
{
	(void)state;
	ove::Stream<128> a(1);
	assert_true(a.valid());

	ove::Stream<128> b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_stream_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Stream<64>>::value,
		      "Stream must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Stream<64>>::value,
		      "Stream must not be copy assignable");
}

/* Method-return-type pins.  Catches an accidental revert of the
 * `try_send_for/until` / `try_receive_for/until` migration from
 * `int` to `Result<void>` at compile time.  Out-param ordering
 * preserved (`size_t&` stays at the end of the bounded forms,
 * matching the prior signature). */
static void test_cpp_stream_return_type_shape(void **state)
{
	(void)state;
	using S = ove::Stream<64>;
	using uchar_p = const void *;
	using void_p = void *;
	static_assert(
		std::is_same_v<decltype(std::declval<S>().send(std::declval<uchar_p>(), size_t{},
							       std::declval<size_t &>())),
			       void>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().try_send(std::declval<uchar_p>(), size_t{},
								   std::declval<size_t &>())),
			       bool>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().try_send_for(
				       std::declval<uchar_p>(), size_t{},
				       std::chrono::milliseconds{1}, std::declval<size_t &>())),
			       ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().try_send_until(
				       std::declval<uchar_p>(), size_t{},
				       std::chrono::steady_clock::now(), std::declval<size_t &>())),
			       ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().receive(std::declval<void_p>(), size_t{},
								  std::declval<size_t &>())),
			       void>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().try_receive(
				       std::declval<void_p>(), size_t{}, std::declval<size_t &>())),
			       bool>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().try_receive_for(
				       std::declval<void_p>(), size_t{},
				       std::chrono::milliseconds{1}, std::declval<size_t &>())),
			       ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().try_receive_until(
				       std::declval<void_p>(), size_t{},
				       std::chrono::steady_clock::now(), std::declval<size_t &>())),
			       ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().send_from_isr(
				       std::declval<uchar_p>(), size_t{}, std::declval<size_t &>())),
			       ove::Result<void>>);
	static_assert(
		std::is_same_v<decltype(std::declval<S>().receive_from_isr(
				       std::declval<void_p>(), size_t{}, std::declval<size_t &>())),
			       ove::Result<void>>);
}

int test_cpp_stream_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_stream_create_destroy),
		cmocka_unit_test(test_cpp_stream_send_receive),
		cmocka_unit_test(test_cpp_stream_bytes_available),
		cmocka_unit_test(test_cpp_stream_reset),
		cmocka_unit_test(test_cpp_stream_send_from_isr),
		cmocka_unit_test(test_cpp_stream_receive_from_isr),
		cmocka_unit_test(test_cpp_stream_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_stream_move_construct),
#endif
		cmocka_unit_test(test_cpp_stream_not_copyable),
		cmocka_unit_test(test_cpp_stream_return_type_shape),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
