#include "framework/ove_test.h"
#include <stdio.h>
#include <stdlib.h>

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void) {}

int main(void)
{
	int failures = 0;

	/*
	 * FS tests skipped — Zephyr native_sim FS stubs conflict with Zephyr
	 * kernel FS APIs. Stub-only suites (networking helpers, I2C/SPI/UART,
	 * PM, etc.) run from the stub target only.
	 */
#define OVE_SUITE(name, label) \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	exit(failures ? 1 : 0);
}
