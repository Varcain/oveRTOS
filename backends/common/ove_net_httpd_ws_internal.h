/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Between the httpd server (ove_net_httpd.c) and its WebSocket support
 * (ove_net_httpd_ws.c); not part of the public API.
 */

#ifndef OVE_NET_HTTPD_WS_INTERNAL_H
#define OVE_NET_HTTPD_WS_INTERNAL_H

#ifdef CONFIG_OVE_NET_HTTPD_WS

/* Creates the connection pool's lock; ove_httpd_start() calls it before
 * the server thread exists.  Idempotent. */
int ove_httpd_ws_init(void);

/* The httpd thread, on its way out: sends each open connection a close
 * frame (1001), calls its on_close and frees it. */
void ove_httpd_ws_close_all(void);

#endif /* CONFIG_OVE_NET_HTTPD_WS */

#endif /* OVE_NET_HTTPD_WS_INTERNAL_H */
