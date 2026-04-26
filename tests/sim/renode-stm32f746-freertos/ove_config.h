/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * oveRTOS feature flags for the Renode STM32F746 heap-mode test firmware.
 * Identical to the zero-heap variant except CONFIG_OVE_ZERO_HEAP is off,
 * which re-enables the heap-backed _create/_destroy APIs.
 */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_RTOS_FREERTOS 1
#define CONFIG_OVE_THREAD 1
#define CONFIG_OVE_APP 1
#define CONFIG_OVE_SYNC 1
#define CONFIG_OVE_QUEUE 1
#define CONFIG_OVE_TIMER 1
#define CONFIG_OVE_EVENTGROUP 1
#define CONFIG_OVE_WORKQUEUE 1
#define CONFIG_OVE_STREAM 1
#define CONFIG_OVE_CONSOLE 1
#define CONFIG_OVE_TIME 1
#define CONFIG_OVE_WATCHDOG 1
#define CONFIG_OVE_NVS 1
#define CONFIG_OVE_SHELL 1
#define CONFIG_OVE_AUDIO 1
#define CONFIG_OVE_BSP 1
#define CONFIG_OVE_BOARD 1
#define CONFIG_OVE_GPIO 1
#define CONFIG_OVE_LED 1
#define CONFIG_OVE_FS 1
#define CONFIG_OVE_LVGL 1
#define CONFIG_OVE_I2C 1
#define CONFIG_OVE_SPI 1
#define CONFIG_OVE_UART 1
#define CONFIG_OVE_NET 1
#define CONFIG_OVE_APP_NAME "test-renode-stm32f746"
#define CONFIG_OVE_APP_VERSION "0.0.0"

#endif
