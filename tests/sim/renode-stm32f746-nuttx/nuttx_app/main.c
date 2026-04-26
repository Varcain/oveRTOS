/*
 * NuttX test runner entry point — Renode/STM32F746 heap mode.
 *
 * Returns from main() rather than calling semihosting_exit().  See the
 * zero-heap sibling main.c for the Renode-specific rationale (NuttX's
 * panic path on Renode 1.16's STM32F7 freezes virtual time).
 */

#include "framework/ove_test.h"
#include <stdio.h>

void ove_main(void) {}

int main(int argc, char *argv[])
{
	int failures = 0;

	(void)argc;
	(void)argv;

#define OVE_SUITE(name, label) \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	return failures ? 1 : 0;
}
