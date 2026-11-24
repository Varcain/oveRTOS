/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "cpp/framework/ove_test.hpp"
#include <cstdio>
#include <cstdlib>

static void run_mutex_suite(void *)
{
	/* During scheduling, only the runner may print or allocate through libc in test code. */
	std::printf("=== C++ Mutex Tests ===\n");
	int count = test_cpp_mutex_run();
	std::printf("\n=== Summary: %d test group(s) had failures ===\n", count);
	std::fflush(stdout);
	/* Exit directly to avoid racing the POSIX scheduler shutdown. */
	std::_Exit(count == 0 ? 0 : 1);
}

int main()
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	static StaticTask_t task;
	static StackType_t stack[configMINIMAL_STACK_SIZE];
	if (xTaskCreateStatic(run_mutex_suite, "tests", configMINIMAL_STACK_SIZE, nullptr,
			      tskIDLE_PRIORITY + 1, stack, &task) == nullptr)
		return 1;
	vTaskStartScheduler();
	return 1;
}
