/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * MQTT 3.1.1 §4.7 topic filter matching — extracted so it can be
 * unit-tested without dragging in the full MQTT client (sockets,
 * TLS, state machine).
 */

#ifndef OVE_MQTT_TOPIC_H
#define OVE_MQTT_TOPIC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Match a topic against a subscription filter with MQTT wildcards.
 *   '+' matches exactly one topic level.
 *   '#' matches zero or more remaining levels (must be last).
 * Returns 1 on match, 0 otherwise.
 */
int ove_mqtt_topic_matches(const char *filter, size_t flen,
                           const char *topic, size_t tlen);

#ifdef __cplusplus
}
#endif

#endif /* OVE_MQTT_TOPIC_H */
