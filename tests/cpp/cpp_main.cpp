#include "framework/ove_test.hpp"
#include <cstdio>

/* Stub ove_main for linking (ove_app.c references it) */
extern "C" void ove_main(void)
{
}

int main(void)
{
	int failures = 0;

	/* Dispatch every suite listed in framework/suites.inc (the single
	 * source of truth — see that file).  Adding a suite there is the only
	 * edit needed; CMake validates the .inc against the test_*.cpp files. */
#define OVE_CPP_SUITE(name, label)               \
	printf("=== C++ " label " Tests ===\n"); \
	failures += test_cpp_##name##_run();
#include "framework/suites.inc"
#undef OVE_CPP_SUITE

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	return failures ? 1 : 0;
}
