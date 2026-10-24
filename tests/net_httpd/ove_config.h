/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * ove_config.h for the httpd/WebSocket host harness: only the modules the
 * server, its built-in routes and the shell need (tests/ove_config.h turns on
 * audio, LVGL and the Linux personality, which routes.c would then pull in).
 */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_RTOS_POSIX 1
#define CONFIG_OVE_THREAD 1
#define CONFIG_OVE_SYNC 1
#define CONFIG_OVE_CONSOLE 1
#define CONFIG_OVE_LOG 1
#define CONFIG_OVE_TIME 1
#define CONFIG_OVE_BSP 1
#define CONFIG_OVE_BOARD 1
#define CONFIG_OVE_GPIO 1
#define CONFIG_OVE_LED 1
#define CONFIG_OVE_SHELL 1
#define CONFIG_OVE_NET 1
#define CONFIG_OVE_NET_HTTPD 1
#define CONFIG_OVE_NET_HTTPD_WS 1
#define CONFIG_OVE_NET_HTTPD_MAX_BODY 1024
#define CONFIG_OVE_APP_NAME "httpd_ws"
#define CONFIG_OVE_APP_VERSION "0.0.0"
#define OVE_LOG_LEVEL 2

#endif /* OVE_CONFIG_H */
