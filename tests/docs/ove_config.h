/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Config for the documentation snippet check (ove lint, doc-snippets): the
 * host test config plus the modules the docs use that it leaves off.
 */

#ifndef OVE_DOCS_CONFIG_H
#define OVE_DOCS_CONFIG_H

#include "../ove_config.h"

#define CONFIG_OVE_BLOCK 1
#define CONFIG_OVE_INFER 1
#define CONFIG_OVE_UART 1
#define CONFIG_OVE_SPI 1
#define CONFIG_OVE_I2C 1
/* tests/CMakeLists.txt turns networking on per target; the cookbook needs it
 * everywhere. Sizes are the Kconfig defaults. */
#define CONFIG_OVE_NET 1
#define CONFIG_OVE_NET_TLS 1
#define CONFIG_OVE_NET_TLS_HEAP_SIZE 32768
#define CONFIG_OVE_NET_HTTP 1
#define CONFIG_OVE_NET_HTTP_MAX_RESPONSE 8192
#define CONFIG_OVE_NET_SNTP 1
#define CONFIG_OVE_NET_MQTT 1
#define CONFIG_OVE_NET_MQTT_MAX_SUBS 8
#define CONFIG_OVE_NET_MQTT_RX_BUF 1024
#define CONFIG_OVE_NET_MQTT_TX_BUF 512
#define CONFIG_OVE_NET_HTTPD 1
#define CONFIG_OVE_NET_HTTPD_WS 1

#endif /* OVE_DOCS_CONFIG_H */
