/*
 * NuttX QEMU test runner entry point (zero-heap mode).
 * Runs as a NuttX application (INIT_ENTRYPOINT) on MPS2-AN500.
 * NuttX provides the POSIX layer; backend modules use pthreads/mqueue/timers.
 */

#include "framework/ove_test.h"
#include "framework/semihosting_exit.h"
#include <stdio.h>
#include <stdlib.h>

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void) {}

int main(int argc, char *argv[])
{
	int failures = 0;

	(void)argc;
	(void)argv;

	printf("=== Thread Tests ===\n");
	failures += test_thread_run();

	printf("=== Sync: Mutex Tests ===\n");
	failures += test_sync_mutex_run();

	printf("=== Sync: Semaphore Tests ===\n");
	failures += test_sync_sem_run();

	printf("=== Sync: Event Tests ===\n");
	failures += test_sync_event_run();

	printf("=== Sync: Condvar Tests ===\n");
	failures += test_sync_condvar_run();

	printf("=== Sync: Recursive Mutex Tests ===\n");
	failures += test_sync_recursive_run();

	printf("=== Queue Tests ===\n");
	failures += test_queue_run();

	printf("=== Timer Tests ===\n");
	failures += test_timer_run();

	printf("=== Time Tests ===\n");
	failures += test_time_run();

	printf("=== EventGroup Tests ===\n");
	failures += test_eventgroup_run();

	printf("=== Workqueue Tests ===\n");
	failures += test_workqueue_run();

	printf("=== Stream Tests ===\n");
	failures += test_stream_run();

	printf("=== Console Tests ===\n");
	failures += test_console_run();

	printf("=== Watchdog Tests ===\n");
	failures += test_watchdog_run();

	printf("=== NVS Tests ===\n");
	failures += test_nvs_run();

	printf("=== Shell Tests ===\n");
	failures += test_shell_run();

	printf("=== Audio Tests ===\n");
	failures += test_audio_run();

	printf("=== BSP Tests ===\n");
	failures += test_bsp_run();

	printf("=== Board Tests ===\n");
	failures += test_board_run();

	printf("=== GPIO Tests ===\n");
	failures += test_gpio_run();

	printf("=== LED Tests ===\n");
	failures += test_led_run();

	/* FS tests skipped — no filesystem on bare-metal QEMU */

	printf("=== LVGL Tests ===\n");
	failures += test_lvgl_run();

	printf("=== App Tests ===\n");
	failures += test_app_run();

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	semihosting_exit(failures ? 1 : 0);
}
