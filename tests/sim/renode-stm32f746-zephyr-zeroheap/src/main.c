#include "framework/ove_test.h"
#include "framework/semihosting_exit.h"
#include <stdio.h>
#include <stdlib.h>

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
	/* Same path on Renode and HW.  Renode 1.16's semihosting handler
	 * doesn't implement SYS_EXIT_EXTENDED, so calling
	 * `semihosting_exit` here drops the CPU into a fault loop after
	 * the bkpt — fault handler accesses tank Renode's wall:sim ratio
	 * and the harness times out at 300 s.  Returning from main runs
	 * Zephyr's k_thread_abort path which is just-as-broken on this
	 * model under zero-heap.  Halt instead — `emulation RunFor` then
	 * advances cleanly to its budget and quit unwinds normally; on
	 * HW the runner has already detected the summary line and is
	 * about to close the serial port. */
	(void)failures;
	for (;;) {
	}
}
