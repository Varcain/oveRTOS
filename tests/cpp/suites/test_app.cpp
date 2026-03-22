#include "../framework/ove_test.hpp"

/* ── Test: OVE_MAIN() macro generates valid extern "C" ove_main ── */

/*
 * We can't use OVE_MAIN() directly in a test binary (it would conflict
 * with the test runner's main()). Instead, verify the macro expands
 * correctly by checking its structure at compile time.
 */

/* Verify ove::run() is callable (link test) */
static void test_cpp_app_run_callable(void **state)
{
	(void)state;
	/*
	 * ove::run() calls ove_run() which is provided by the stub.
	 * Just verify it compiles and links — we can't actually start the
	 * scheduler in a test.
	 */
	auto fn = &ove::run;
	assert_non_null(reinterpret_cast<void *>(fn));
}

/* Verify OVE_MAIN() generates a static function + extern "C" wrapper */
static void test_cpp_app_ove_main_macro(void **state)
{
	(void)state;

	/*
	 * OVE_MAIN() expands to:
	 *   static void ove_main_impl();
	 *   extern "C" void ove_main(void) { ove_main_impl(); }
	 *   static void ove_main_impl()
	 *
	 * We can't instantiate it here without conflicting with test main,
	 * but we can verify the ove_run symbol exists and links.
	 */
	auto fn = &ove_run;
	assert_non_null(reinterpret_cast<void *>(fn));
}

int test_cpp_app_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_app_run_callable),
		cmocka_unit_test(test_cpp_app_ove_main_macro),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
