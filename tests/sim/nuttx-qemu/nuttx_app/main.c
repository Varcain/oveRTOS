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
#include <sys/mount.h>
extern void __gcov_dump(void);
#endif

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void)
{
}

int main(int argc, char *argv[])
{
	int failures = 0;

	(void)argc;
	(void)argv;

#ifdef OVE_COVERAGE
	/* Mount the host root at /host so libgcov's fopen("/abs/path.gcda")
	 * can reach the host filesystem via the ARM semihosting trap. We
	 * also bind / itself via GCOV_PREFIX_STRIP so gcda paths rooted at
	 * the host / are written under /host/... */
	if (mount(NULL, "/host", "hostfs", 0, "fs=/") != 0) {
		printf("WARN: hostfs mount failed; gcda will not be captured\n");
	}
#endif

	/* FS tests skipped — no filesystem mount on MPS2-AN500 QEMU */
#define OVE_SUITE(name, label)               \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
#ifdef OVE_COVERAGE
	/* libgcov writes .gcda via fopen — with hostfs mounted at /host,
	 * set GCOV_PREFIX=/host so "/host" + source-abs-path produces a
	 * valid NuttX VFS path that passes through semihosting to the
	 * real host filesystem. */
	setenv("GCOV_PREFIX", "/host", 1);
	setenv("GCOV_PREFIX_STRIP", "0", 1);
	__gcov_dump();
#endif
#ifdef CONFIG_ARCH_SIM
	return failures ? 1 : 0;
#else
	semihosting_exit(failures ? 1 : 0);
#endif
}
