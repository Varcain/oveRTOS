/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_EVENTGROUP_VALIDATE_H
#define OVE_EVENTGROUP_VALIDATE_H
#include <stdbool.h>
#include "ove/eventgroup.h"

static inline bool ove_eventgroup_mask_valid(ove_eventbits_t bits)
{
	return (bits & ~ove_eventgroup_valid_mask()) == 0;
}
static inline bool ove_eventgroup_wait_input_valid(ove_eventbits_t bits, uint32_t flags)
{
	return bits != 0 && ove_eventgroup_mask_valid(bits) &&
	       (flags & ~(uint32_t)(OVE_EG_WAIT_ALL | OVE_EG_CLEAR_ON_EXIT)) == 0;
}
#endif
