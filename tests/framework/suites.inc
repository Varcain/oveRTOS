/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Authoritative list of test suites. X-macro style: callers define zero or
 * more of the OVE_SUITE_* macros and include this header. Undefined macros
 * are treated as no-ops, so a target only opts in to the suite categories
 * it can actually run.
 *
 *   OVE_SUITE(name, label)       — always runnable
 *   OVE_SUITE_FS(name, label)    — needs a filesystem backend
 *   OVE_SUITE_STUB(name, label)  — stub-only (pure host/C tests)
 *
 * Example:
 *     #define OVE_SUITE(n, l) \
 *         printf("=== " l " Tests ===\n"); failures += test_##n##_run();
 *     #define OVE_SUITE_FS(n, l) OVE_SUITE(n, l)
 *     #include "framework/suites.inc"
 */

#ifndef OVE_SUITE
#define OVE_SUITE(name, label)
#define _OVE_SUITES_INC_DEFAULTED_CORE
#endif
#ifndef OVE_SUITE_FS
#define OVE_SUITE_FS(name, label)
#define _OVE_SUITES_INC_DEFAULTED_FS
#endif
#ifndef OVE_SUITE_STUB
#define OVE_SUITE_STUB(name, label)
#define _OVE_SUITES_INC_DEFAULTED_STUB
#endif

/* hw_stm32f746 must be first — its watchdog test resets the MCU on
 * boot #1.  See tests/suites/test_hw_stm32f746.c for the reset-and-
 * resume protocol.  No-op skip on non-HW targets. */
OVE_SUITE(hw_stm32f746,    "HW STM32F746 (real silicon)")
OVE_SUITE(storage_bounds,  "Storage Bounds")
OVE_SUITE(renode_stm32_obs,"Renode STM32 Observability")
OVE_SUITE(renode_stm32_periph, "Renode STM32 Peripherals (I2C/UART/SPI)")
OVE_SUITE(renode_stm32_net, "Renode STM32 Networking")
OVE_SUITE(thread,          "Thread")
OVE_SUITE(sync_mutex,      "Sync: Mutex")
OVE_SUITE(sync_sem,        "Sync: Semaphore")
OVE_SUITE(sync_event,      "Sync: Event")
OVE_SUITE(sync_condvar,    "Sync: Condvar")
OVE_SUITE(sync_recursive,  "Sync: Recursive Mutex")
OVE_SUITE(queue,           "Queue")
OVE_SUITE(timer,           "Timer")
OVE_SUITE(time,            "Time")
OVE_SUITE(eventgroup,      "EventGroup")
OVE_SUITE(workqueue,       "Workqueue")
OVE_SUITE(stream,          "Stream")
OVE_SUITE(public_create,   "Public Create API")
OVE_SUITE(console,         "Console")
OVE_SUITE(watchdog,        "Watchdog")
OVE_SUITE(nvs,             "NVS")
OVE_SUITE(shell,           "Shell")
OVE_SUITE(audio,           "Audio")
OVE_SUITE(bsp,             "BSP")
OVE_SUITE(board,           "Board")
OVE_SUITE(gpio,            "GPIO")
OVE_SUITE(led,             "LED")
OVE_SUITE(lvgl,            "LVGL")
OVE_SUITE(app,             "App")

OVE_SUITE_FS(fs, "FS")

OVE_SUITE_STUB(static_define, "Static Define")
OVE_SUITE_STUB(infer,         "Inference")
OVE_SUITE_STUB(net_mqtt,      "Net: MQTT Wildcard")
OVE_SUITE_STUB(net_httpd,     "Net: HTTPD (SHA1/Base64)")
OVE_SUITE_STUB(net_sntp,      "Net: SNTP")
OVE_SUITE_STUB(net_loopback,  "Net: TCP Loopback")
OVE_SUITE_STUB(i2c,           "I2C")
OVE_SUITE_STUB(spi,           "SPI")
OVE_SUITE_STUB(uart,          "UART")
OVE_SUITE_STUB(pm,            "PM")

#ifdef _OVE_SUITES_INC_DEFAULTED_CORE
#undef OVE_SUITE
#undef _OVE_SUITES_INC_DEFAULTED_CORE
#endif
#ifdef _OVE_SUITES_INC_DEFAULTED_FS
#undef OVE_SUITE_FS
#undef _OVE_SUITES_INC_DEFAULTED_FS
#endif
#ifdef _OVE_SUITES_INC_DEFAULTED_STUB
#undef OVE_SUITE_STUB
#undef _OVE_SUITES_INC_DEFAULTED_STUB
#endif
