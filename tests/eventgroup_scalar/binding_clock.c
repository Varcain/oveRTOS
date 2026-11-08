/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/time.h"
static unsigned clocks;
unsigned eventgroup_test_clocks(void)
{
	return clocks;
}
int ove_time_get_ns(uint64_t *out)
{
	++clocks;
	*out = 10;
	return OVE_OK;
}
