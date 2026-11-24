/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

void *__wrap_malloc(size_t size);
void __wrap_free(void *ptr);
void *__wrap_realloc(void *ptr, size_t size);

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

int main(void)
{
	static void *blocks[configTOTAL_HEAP_SIZE / 32];
	size_t count = 0;
	/* heap_4's first-fit allocations leave the last block at the end of the heap. */
	while (count < sizeof(blocks) / sizeof(blocks[0]) &&
	       (blocks[count] = __wrap_malloc(32)) != NULL) {
		count++;
	}
	configASSERT(count > 0);
	void *last = blocks[count - 1];
	memset(last, 0xA5, 32);
	for (size_t i = 0; i + 1 < count; i++) {
		__wrap_free(blocks[i]);
	}
	unsigned char *grown = __wrap_realloc(last, 4096);
	configASSERT(grown != NULL);
	for (size_t i = 0; i < 32; i++) {
		configASSERT(grown[i] == 0xA5);
	}
	__wrap_free(grown);

	/* Shrink into the last slot at the end of the heap. */
	unsigned char *big = __wrap_malloc(4096);
	configASSERT(big != NULL);
	memset(big, 0x5A, 4096);
	count = 0;
	while (count < sizeof(blocks) / sizeof(blocks[0]) &&
	       (blocks[count] = __wrap_malloc(32)) != NULL) {
		count++;
	}
	configASSERT(count > 0);
	__wrap_free(blocks[count - 1]);
	unsigned char *shrunk = __wrap_realloc(big, 16);
	configASSERT(shrunk == blocks[count - 1]);
	for (size_t i = 0; i < 16; i++) {
		configASSERT(shrunk[i] == 0x5A);
	}
	__wrap_free(shrunk);
	for (size_t i = 0; i + 1 < count; i++) {
		__wrap_free(blocks[i]);
	}
	puts("FreeRTOS libc realloc grow and shrink: PASS");
	return 0;
}
