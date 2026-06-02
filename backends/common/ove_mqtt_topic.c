/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_mqtt_topic.h"

/*
 * '#' is the MQTT multi-level wildcard only as the *final* filter character,
 * and only at the start of the filter or immediately after a '/'.  Anywhere
 * else it is an ordinary character: a malformed filter like "a#b" or "a#"
 * must NOT match every topic (MQTT 3.1.1 §4.7.1.2).
 */
static int is_multilevel_wildcard(const char *filter, size_t flen, size_t fi)
{
	return filter[fi] == '#' && fi + 1 == flen && (fi == 0 || filter[fi - 1] == '/');
}

int ove_mqtt_topic_matches(const char *filter, size_t flen, const char *topic, size_t tlen)
{
	size_t fi = 0, ti = 0;

	while (fi < flen && ti < tlen) {
		if (is_multilevel_wildcard(filter, flen, fi))
			return 1;

		if (filter[fi] == '+') {
			while (ti < tlen && topic[ti] != '/')
				ti++;
			fi++;
			continue;
		}

		if (filter[fi] != topic[ti])
			return 0;

		fi++;
		ti++;
	}

	if (fi == flen && ti == tlen)
		return 1;

	if (fi + 1 < flen && filter[fi] == '/' && filter[fi + 1] == '#')
		return (ti == tlen);

	if (fi < flen && is_multilevel_wildcard(filter, flen, fi))
		return 1;

	return 0;
}
