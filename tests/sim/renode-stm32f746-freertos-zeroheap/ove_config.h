/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * oveRTOS feature flags for the Renode STM32F746 zero-heap test firmware.
 * Mirrors the QEMU zero-heap test config with the RTOS set to FreeRTOS
 * and the backend gated to the STM32F7 variant.
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
#define CONFIG_OVE_LVGL 1
#define CONFIG_OVE_ZERO_HEAP 1
/* Hardware peripheral APIs — drivers in drivers/freertos/stm32f7/ +
 * dispatchers in src/ove_<periph>.c, exercised against Renode's
 * STM32F7_I2C / STM32SPI / STM32F7_USART models. */
#define CONFIG_OVE_I2C 1
#define CONFIG_OVE_SPI 1
#define CONFIG_OVE_UART 1
/* Networking — freertos_net.c + stm32f7_eth.c + lwIP, exercised against
 * Renode's Network.SynopsysEthernetMAC + EthernetPhysicalLayer. */
#define CONFIG_OVE_NET 1
#define CONFIG_OVE_ASYNC 1
#define CONFIG_OVE_APP_NAME "test-renode-stm32f746"
#define CONFIG_OVE_APP_VERSION "0.0.0"

#endif
