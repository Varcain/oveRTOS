/*
 * NuttX QEMU test runner entry point.
 * Runs as a NuttX application (INIT_ENTRYPOINT) on MPS2-AN500.
 * NuttX provides the POSIX layer; backend modules use pthreads/mqueue/timers.
 */

#include "framework/ove_test.h"
#ifdef CONFIG_ARCH_SIM
#include <stdlib.h>
#else
#include "framework/semihosting_exit.h"
#endif
#include <stdio.h>

#ifdef OVE_COVERAGE
extern void __gcov_dump(void);
#endif

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void) {}

int main(int argc, char *argv[])
{
	int failures = 0;

	(void)argc;
	(void)argv;

	/* FS tests skipped — no filesystem mount on MPS2-AN500 QEMU */
#define OVE_SUITE(name, label) \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
#ifdef OVE_COVERAGE
	__gcov_dump();
#endif
#ifdef CONFIG_ARCH_SIM
	return failures ? 1 : 0;
#else
	semihosting_exit(failures ? 1 : 0);
#endif
}
