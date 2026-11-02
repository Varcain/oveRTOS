/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H
#define CONFIG_OVE_RTOS_FREERTOS 1
#define CONFIG_OVE_SYNC 1
#define CONFIG_OVE_SPI 1
#define CONFIG_OVE_GPIO 1
#define CONFIG_OVE_ASYNC 1
#ifdef OVE_RUST_SPI_TEST
#define CONFIG_OVE_TIME 1
#define CONFIG_OVE_TIMER 1
#define CONFIG_OVE_CONSOLE 1
#endif
#endif
