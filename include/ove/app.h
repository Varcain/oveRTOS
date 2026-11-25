/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file app.h
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
 * The application must implement this function.  It creates its bootstrap
 * resources and then calls ove_run() to apply the configured heap policy and
 * start the scheduler.  Runtime tasks may create further resources when the
 * selected heap policy permits it.
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
 *       (scheduler reclaims the main stack); on NuttX/Zephyr it
 *       is latent but still UB.
 *
 * @see ove_run, ove_app_run
 */
extern void ove_main(void);

/**
 * @brief Apply the heap policy and start the RTOS scheduler.
 *
 * Call this from ove_main() after declaring static resources and
 * spawning boot-time helpers. In zero-heap builds, @c ove_run calls
 * @ref ove_heap_lock before starting the scheduler to apply the
 * backend's allocation policy.
 *
 * Apps whose runtime structurally requires post-init dynamic
 * allocation (the benchmark suite, dynamic worker pools, etc.) skip
 * `ove_run` and call @ref ove_thread_start_scheduler directly to
 * opt out of the lock.
 *
 * On most platforms the scheduler never returns and this function
 * blocks forever.
 *
 * Call from application startup, not a thread entry or native pthread worker.
 * NuttX ignores calls from those workers without locking the heap.
 *
 * @see ove_main, ove_heap_lock, ove_thread_start_scheduler
 */
void ove_run(void);

/**
 * @brief Finish the application using the platform's product-level policy.
 *
 * On QEMU MPS2 AN500 and AN521, request a semihosted exit with @p status.
 * On STM32F746G-DISCO, request a system reset. The function spins forever on
 * other targets or if the exit or reset request returns.
 *
 * Call this only when the entire firmware application has finished or cannot
 * continue.
 *
 * @param[in] status Semihosted exit status. Zero for success, nonzero for failure.
 *
 * @note This function never returns.
 */
void ove_app_exit(unsigned int status) __attribute__((noreturn));

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
 * @brief Apply the zero-heap allocation policy after initialization.
 *
 * Call after declaring static resources and spawning boot-time helpers.
 * After the lock, NuttX serves wrapped libc and kernel allocations from a
 * fixed-size reserve.
 * If an allocation from the reserve fails, the wrapper asserts with
 * @c CONFIG_DEBUG_ASSERTIONS outside test mode and returns NULL otherwise.
 *
 * FreeRTOS wrapped libc allocations return NULL after the lock.
 * Zephyr zero-heap applications have no kernel heap and do not wrap the
 * libc allocators, so this function has no effect there. It also has no
 * effect in heap builds.
 *
 * @c ove_run invokes this when @c CONFIG_OVE_ZERO_HEAP is enabled.
 */
void ove_heap_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* OVE_APP_H */

/** @} */
