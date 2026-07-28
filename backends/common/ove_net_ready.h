/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Private network-backend readiness channel. A stack/driver publishes that
 * received traffic may have changed socket readiness; one host adapter may
 * subscribe and translate the notification into its own scheduler primitive.
 */
#ifndef OVE_NET_READY_H
#define OVE_NET_READY_H

typedef void (*ove_net_ready_callback_t)(void);

int ove_net_ready_subscribe(ove_net_ready_callback_t callback);
void ove_net_ready_unsubscribe(ove_net_ready_callback_t callback);
void ove_net_ready_publish(void);

#endif /* OVE_NET_READY_H */
