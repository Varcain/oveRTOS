/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Test-only ove_config.h — enables all modules for stub testing.
 */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_RTOS_POSIX 1
#define CONFIG_OVE_THREAD 1
#define CONFIG_OVE_APP 1
#define CONFIG_OVE_SYNC 1
#define CONFIG_OVE_AUDIO 1
#define CONFIG_OVE_FS 1
#define CONFIG_OVE_CONSOLE 1
#define CONFIG_OVE_LOG 1
#define CONFIG_OVE_TIME 1
#define CONFIG_OVE_BSP 1
#define CONFIG_OVE_BOARD 1
#define CONFIG_OVE_GPIO 1
#define CONFIG_OVE_LED 1
#define CONFIG_OVE_LVGL 1
#define CONFIG_OVE_QUEUE 1
#define CONFIG_OVE_TIMER 1
#define CONFIG_OVE_EVENTGROUP 1
#define CONFIG_OVE_SHELL 1
#define CONFIG_OVE_NVS 1
#define CONFIG_OVE_WATCHDOG 1
#define CONFIG_OVE_WORKQUEUE 1
#define CONFIG_OVE_STREAM 1
#define CONFIG_OVE_PM 1
#define CONFIG_OVE_PM_MAX_WAKE_SOURCES 8
#define CONFIG_OVE_PM_MAX_NOTIFIERS 4

#define CONFIG_OVE_APP_NAME "test"
#define CONFIG_OVE_APP_VERSION "0.0.0"
#define OVE_LOG_LEVEL 0

#endif /* OVE_CONFIG_H */
