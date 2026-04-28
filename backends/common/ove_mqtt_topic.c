/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_mqtt_topic.h"

int ove_mqtt_topic_matches(const char *filter, size_t flen, const char *topic, size_t tlen)
{
	size_t fi = 0, ti = 0;

	while (fi < flen && ti < tlen) {
		if (filter[fi] == '#')
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

	if (fi < flen && filter[fi] == '#')
		return 1;

	return 0;
}
