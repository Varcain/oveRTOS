#include "framework/ove_test.h"
#include "framework/semihosting_exit.h"
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
	semihosting_exit(failures ? 1 : 0);
}
