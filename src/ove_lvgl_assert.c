/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * LVGL assert handler. An LVGL assert (most often an exhausted LV_MEM_SIZE pool)
 * used to spin without a word; now it names the source line before it halts.
 */

#include "ove/lvgl_assert.h"
#include "ove/log.h"

void ove_lvgl_assert_failed(const char *file, int line)
{
	OVE_LOG_ERR("LVGL assert at %s:%d", file, line);
	for (;;)
		;
}
