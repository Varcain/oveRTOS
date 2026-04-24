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
 * @note On FreeRTOS, `vTaskStartScheduler()` inside ove_run() repurposes the
 *       main task's stack once it switches to the first user task. Any
 *       object declared as a local in `ove_main()` and later referenced
 *       by a worker thread will be clobbered.  Long-lived resources
 *       (audio graphs, DSP state, queues) must live at file scope as
 *       `static` storage, not on the `ove_main()` stack frame.
 *
 * @see ove_run, ove_app_run
 */
extern void ove_main(void);

/**
 * @brief Start optional subsystems (e.g. audio) and launch the RTOS scheduler.
 *
 * Call this from ove_main() after all resources have been created.  On most
 * platforms the scheduler never returns and this function blocks forever.
 *
 * @see ove_main
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

#ifdef __cplusplus
}
#endif

#endif /* OVE_APP_H */

/** @} */
