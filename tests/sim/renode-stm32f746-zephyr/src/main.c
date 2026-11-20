#include "framework/ove_test.h"
#include <stdio.h>
#include <stdlib.h>

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void)
{
}

int main(void)
{
	int failures = 0;

	/* FS tests skipped — no filesystem on bare-metal QEMU */
#define OVE_SUITE(name, label)               \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	/* Halt after the summary so Renode's RunFor can finish.
	 * Renode 1.16 lacks SYS_EXIT_EXTENDED, so the CPU falls into
	 * a fault loop after the trap.  Returning from main calls
	 * k_thread_abort, which fails on this model in zero-heap mode. */
	(void)failures;
	for (;;) {
	}
}
