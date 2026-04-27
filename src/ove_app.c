/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"
#include "ove/ove.h"

int ove_app_run(void)
{
#ifdef CONFIG_OVE_BOARD
	ove_board_init();
#endif

#ifdef CONFIG_OVE_CONSOLE
	ove_console_init();
#endif

	OVE_LOG("ove: starting %s %s on %s\n",
		    CONFIG_OVE_APP_NAME,
		    CONFIG_OVE_APP_VERSION,
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
