/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "framework/ove_test.h"
#include <stdio.h>

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void) {}

int main(void)
{
	int failures = 0;

	/*
	 * Functional tests run with stub backend (direct-linked).
	 */
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

	printf("=== FS Tests ===\n");
	failures += test_fs_run();

	printf("=== LVGL Tests ===\n");
	failures += test_lvgl_run();

	printf("=== Static Define Tests ===\n");
	failures += test_static_define_run();

	printf("=== App Tests ===\n");
	failures += test_app_run();

	printf("=== Inference Tests ===\n");
	failures += test_infer_run();

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	return failures ? 1 : 0;
}
