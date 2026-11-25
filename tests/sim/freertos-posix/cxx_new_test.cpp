/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <cstdint>
#include <cstdio>
#include <new>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"

void vTaskSuspendAll(void)
{
}
BaseType_t xTaskResumeAll(void)
{
	return pdFALSE;
}
void vPortEnterCritical(void)
{
}
void vPortExitCritical(void)
{
}
}

struct alignas(32) Obj {
	unsigned char bytes[48];
};

/* The block must be aligned and taken from heap_4. */
static void check(const Obj *p, size_t baseline)
{
	configASSERT(p != nullptr);
	configASSERT(reinterpret_cast<std::uintptr_t>(p) % alignof(Obj) == 0);
	configASSERT(xPortGetFreeHeapSize() < baseline);
}

int main()
{
	::operator delete(::operator new(1)); /* initialise heap_4 */
	const size_t baseline = xPortGetFreeHeapSize();

	Obj *one = new Obj;
	check(one, baseline);
	delete one;
	Obj *many = new Obj[3];
	check(many, baseline);
	delete[] many;
	Obj *maybe = new (std::nothrow) Obj;
	check(maybe, baseline);
	delete maybe;

	configASSERT(xPortGetFreeHeapSize() == baseline);
	std::puts("FreeRTOS aligned operator new: PASS");
	return 0;
}
