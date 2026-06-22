/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * oveRTOS config for the Linux-personality interop demo. Selects the Zephyr
 * backend so the demo can use the engine-agnostic oveRTOS RTOS APIs (ove_thread,
 * ove_queue, ove_time) instead of calling Zephyr directly, plus the
 * engine-agnostic layers the Linux personality seam dispatches into (bounded
 * arena + loader + Linux syscall core).
 */

#ifndef OVE_CONFIG_LINUX_INTEROP_H
#define OVE_CONFIG_LINUX_INTEROP_H

#define CONFIG_OVE_RTOS_ZEPHYR 1 /* select the Zephyr backend (storage sizes, primitives) */
#define CONFIG_OVE_THREAD 1	 /* ove_thread_*  */
#define CONFIG_OVE_QUEUE 1	 /* ove_queue_*   */
#define CONFIG_OVE_TIME 1	 /* ove_time_*    */

#define CONFIG_OVE_ARENA 1  /* bounded region allocator (loader/program memory) */
#define CONFIG_OVE_LOADER 1 /* bFLT loader   */
#define CONFIG_OVE_LINUX 1  /* Linux personality syscall core */

#endif /* OVE_CONFIG_LINUX_INTEROP_H */
