/*
 * NuttX test runner entry point — Renode/STM32F746 zero-heap.
 *
 * Diverges from the QEMU sibling: we deliberately don't call
 * semihosting_exit() at the end.  On Renode 1.16 + STM32F7, NuttX's
 * panic path triggered by the bkpt #0xab trap halts the CPU at PC=0,
 * which freezes Renode's virtual clock — `emulation RunFor` then
 * blocks indefinitely instead of timing out.  Returning from main
 * lets NuttX's idle loop run while the harness's `RunFor` budget
 * expires cleanly.
 */

#include "framework/ove_test.h"
#include <stdio.h>
#include <stdlib.h>

void ove_main(void)
{
}

int main(int argc, char *argv[])
{
	int failures = 0;

	(void)argc;
	(void)argv;

#define OVE_SUITE(name, label)               \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	return failures ? 1 : 0;
}
