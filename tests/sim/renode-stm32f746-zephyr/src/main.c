#include "framework/ove_test.h"
#include "framework/semihosting_exit.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef CONFIG_COVERAGE_SEMIHOST
#include <zephyr/debug/gcov.h>
#endif

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void) {}

int main(void)
{
	int failures = 0;

	/* FS tests skipped — no filesystem on bare-metal QEMU */
#define OVE_SUITE(name, label) \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
#ifdef CONFIG_COVERAGE_SEMIHOST
	/* Zephyr normally auto-invokes this when main() returns
	 * (kernel/init.c), but we terminate via the SYS_EXIT semihosting trap
	 * to close the QEMU process, which bypasses that path — so dump
	 * explicitly here. */
	gcov_coverage_semihost();
#endif
	/* See zero-heap sibling for rationale — same halt path on both
	 * Renode and HW avoids the post-summary fault loop that
	 * `semihosting_exit` triggers when Renode 1.16 doesn't catch
	 * SYS_EXIT_EXTENDED.  HW runner exits as soon as it sees the
	 * summary line; Renode's RunFor timer expires cleanly. */
	(void)failures;
	for (;;) {
	}
}
