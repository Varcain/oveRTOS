/*
 * NuttX test runner entry point for Renode/STM32F746.
 *
 * The semihosting_exit() breakpoint triggers NuttX's panic path on
 * Renode 1.16's STM32F7, halting the CPU at PC=0 and freezing virtual
 * time. Returning from main() leaves NuttX's idle loop running so
 * Renode's RunFor can reach its time limit.
 */

#include "framework/ove_test.h"
#include <stdio.h>
#ifndef CONFIG_OVE_ZERO_HEAP
#include <sys/mount.h>
#endif

void ove_main(void)
{
}

int main(int argc, char *argv[])
{
	int failures = 0;

	(void)argc;
	(void)argv;

#ifndef CONFIG_OVE_ZERO_HEAP
	/* tmpfs at /tmp: the file-backed NVS suite keeps its keys there. */
	if (mount(NULL, "/tmp", "tmpfs", 0, NULL) != 0) {
		printf("WARN: tmpfs mount at /tmp failed\n");
	}
#endif

#define OVE_SUITE(name, label)               \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	return failures ? 1 : 0;
}
