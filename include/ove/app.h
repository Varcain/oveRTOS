/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_app Application lifecycle
 * @brief Application entry point and RTOS scheduler startup helpers.
 *
 * The typical call chain is:
 *   platform @c main() → ove_app_run() → ove_main() → ove_run()
 * @{
 */

#ifndef OVE_APP_H
#define OVE_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Application-defined entry point called after board and console init.
 *
 * The application must implement this function.  It is responsible for
 * creating all RTOS resources (threads, queues, timers, …) and then
 * calling ove_run() to start the scheduler.
 *
 * @note This function must not return before calling ove_run().
 *
 * @note **Object lifetime**: anything that worker threads access after
 *       @c ove_main() returns (audio graphs, DSP state, long-lived
 *       buffers) must have storage that outlives this function.  In C
 *       terms: give it `static` storage class — either a `static` local
 *       or a file-scope `static` — or allocate it on the heap.  Plain
 *       automatic locals are popped when @c ove_main() unwinds; any
 *       pointer a worker kept into them becomes dangling.  This is the
 *       same rule that applies whenever a function hands out a pointer
 *       to a local, it's just more visible here because workers
 *       outlive the scope.  On FreeRTOS the failure is immediate
 *       (scheduler reclaims the main stack); on POSIX/NuttX/Zephyr it
 *       is latent but still UB.
 *
 * @see ove_run, ove_app_run
 */
extern void ove_main(void);

/**
 * @brief End the init phase: lock the heap (zero-heap mode) and launch
 *        the RTOS scheduler.
 *
 * Call this from ove_main() after every static resource has been
 * declared and every boot-time helper task spawned.  In zero-heap
 * mode `ove_run` calls @ref ove_heap_lock immediately before kicking
 * off the scheduler, so any subsequent malloc / kmm_malloc /
 * pvPortMalloc traps via DEBUGASSERT (or returns NULL in test mode).
 * Apps whose runtime structurally requires post-init dynamic
 * allocation (the benchmark suite, dynamic worker pools, etc.) skip
 * `ove_run` and call @ref ove_thread_start_scheduler directly to
 * opt out of the lock.
 *
 * On most platforms the scheduler never returns and this function
 * blocks forever.
 *
 * @see ove_main, ove_heap_lock, ove_thread_start_scheduler
 */
void ove_run(void);

/**
 * @brief Platform entry point: initialise the board and then run the application.
 *
 * Called by the platform-specific @c main() after registering any necessary
 * backends.  Internally it performs board and console initialisation and then
 * calls ove_main().
 *
 * @return 0 on success.  On most platforms the scheduler never returns so
 *         this function never actually reaches its @c return statement.
 *
 * @see ove_main
 */
int ove_app_run(void);

/**
 * @brief Lock the kernel heap after init (zero-heap mode safety net).
 *
 * On RTOSes whose kernel-side static configuration cannot fully
 * eliminate boot-time mm allocations (notably NuttX, where
 * @c task_create / @c pthread_create allocate TCBs and stacks from
 * kernel mm), call this after every static resource has been declared.
 * Subsequent kernel allocations trip a @c DEBUGASSERT and abort the
 * binary, so a stray malloc / @c kmm_malloc surfaces immediately
 * during testing instead of hiding behind a sized heap.
 *
 * The function is a no-op on RTOSes whose zero-heap configuration is
 * already provably static (FreeRTOS with
 * @c configSUPPORT_DYNAMIC_ALLOCATION=0, Zephyr with no kernel heap
 * pool).  NuttX implements the lock by setting a flag tested by a
 * @c --wrap=malloc trampoline in @c backends/nuttx/nuttx_heap_lock.c.
 *
 * @c ove_run automatically invokes this when @c CONFIG_OVE_ZERO_HEAP
 * is enabled; applications usually don't need to call it directly.
 */
void ove_heap_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* OVE_APP_H */

/** @} */
