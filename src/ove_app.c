/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"
#include "ove/ove.h"

#if defined(CONFIG_OVE_RTOS_POSIX)
#include <stdlib.h>
#endif

int ove_app_run(void)
{
#ifdef CONFIG_OVE_BOARD
	ove_board_init();
#endif

#ifdef CONFIG_OVE_CONSOLE
	ove_console_init();
#endif

	OVE_LOG("ove: starting %s %s on %s\n", CONFIG_OVE_APP_NAME, CONFIG_OVE_APP_VERSION,
		OVE_RTOS_NAME);

	ove_main();

	return OVE_OK;
}

/*
 * ove_heap_lock — strong impl lives in backends/common/ove_heap_lock.c
 * (linked on FreeRTOS + NuttX where there's a wrap chain to gate).
 * Provide a weak no-op fallback here so Zephyr / POSIX builds still
 * link: those backends have no kernel-mm to trap (Zephyr+zeroheap is
 * provably heap-zero, audited at link time; POSIX is sim only), so
 * the lock has nothing to do anyway.
 */
__attribute__((weak)) void ove_heap_lock(void)
{
}

void ove_run(void)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	/*
	 * Boundary between init and run phases.  In zero-heap mode every
	 * static resource has been declared by ove_main() above (via
	 * OVE_*_DEFINE_STATIC at file scope or ove_*_init() with caller-
	 * supplied storage) and any kernel-mm allocation up to this point
	 * is the documented boot-time carve-out.  After this line, the
	 * trap engages: any subsequent malloc / kmm_malloc / pvPortMalloc
	 * fails with DEBUGASSERT (or returns NULL in test-mode).
	 *
	 * Apps that genuinely need post-init dynamic allocation (the
	 * benchmark suite measuring create/destroy latency, dynamic
	 * worker pools that grow on demand) skip ove_run entirely and
	 * call ove_thread_start_scheduler() directly — that's the
	 * explicit opt-out from the lock.  Building such an app with
	 * CONFIG_OVE_ZERO_HEAP=y is allowed; the lock just isn't engaged.
	 */
	ove_heap_lock();
#endif
	ove_thread_start_scheduler();
}

#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500) || \
	defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
static long app_semihost(unsigned long op, void *arg)
{
	register unsigned long r0 __asm__("r0") = op;
	register void *r1 __asm__("r1") = arg;
	__asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
	return (long)r0;
}
#endif

void ove_app_exit(unsigned int status)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	/* Preserve AIRCR's priority grouping while requesting a system reset.
	 * Barriers match ARM's reset sequence: all earlier memory transactions
	 * complete before SYSRESETREQ and no later access escapes it. */
	volatile unsigned int *const aircr = (volatile unsigned int *)0xE000ED0Cu;
	unsigned int value = (*aircr & (7u << 8)) | 0x05FA0004u;
	__asm__ volatile("dsb 0xf" ::: "memory");
	*aircr = value;
	__asm__ volatile("dsb 0xf" ::: "memory");
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500) || \
	defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
	unsigned long block[2] = {0x20026u /* ADP_Stopped_ApplicationExit */, status};
	(void)app_semihost(0x20 /* SYS_EXIT_EXTENDED */, block);
#elif defined(CONFIG_OVE_RTOS_POSIX)
	exit((int)status);
#else
	(void)status;
#endif
	for (;;) {
	}
}
