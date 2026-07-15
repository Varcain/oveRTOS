/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Internal-memory heap used exclusively by dynamically created FreeRTOS task stacks.
 *
 * Linux-personality builds place the general 128 KiB FreeRTOS heap in external SDRAM.  The
 * privileged host intentionally sees that SDRAM through the Cortex-M7 Device background mapping
 * while guests receive cacheable per-process MPU regions.  Device memory is unsuitable for CPU
 * stacks: compiler-generated unaligned accesses and hardware exception/FP stacking can fault.
 *
 * FreeRTOS supports routing task stacks through pvPortMallocStack()/vPortFreeStack().  Instantiate
 * its proven heap_4 allocator a second time under private symbol names and back it with 32 KiB of
 * internal DTCM.  The linker combines this pool with explicitly tagged static host stacks and
 * rejects configurations that exceed the internal region.
 */

#include "FreeRTOS.h"

#if (configSTACK_ALLOCATION_FROM_SEPARATE_HEAP == 1)

#define OVE_STACK_HEAP_SIZE (32u * 1024u)

uint8_t ucStackHeap[OVE_STACK_HEAP_SIZE]
	__attribute__((section(".host_stacks"), aligned(portBYTE_ALIGNMENT)));

/* Reuse heap_4 without duplicating or modifying the externally maintained FreeRTOS source.  Its
 * file-local allocator state remains independent; rename every exported entry point so only the
 * two standard stack-allocation hooks are visible through portable.h. */
#undef configTOTAL_HEAP_SIZE
#define configTOTAL_HEAP_SIZE OVE_STACK_HEAP_SIZE
#define ucHeap ucStackHeap
#define pvPortMalloc pvPortMallocStack
#define vPortFree vPortFreeStack
#define xPortGetFreeHeapSize xPortGetFreeStackHeapSize
#define xPortGetMinimumEverFreeHeapSize xPortGetMinimumEverFreeStackHeapSize
#define xPortResetHeapMinimumEverFreeHeapSize xPortResetStackHeapMinimumEverFreeHeapSize
#define vPortInitialiseBlocks vPortInitialiseStackHeapBlocks
#define pvPortCalloc pvPortCallocStack
#define vPortGetHeapStats vPortGetStackHeapStats
#define vPortHeapResetState vPortStackHeapResetState

#include "portable/MemMang/heap_4.c"

#endif /* configSTACK_ALLOCATION_FROM_SEPARATE_HEAP */
