/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_I2S

#include "ove/hal/hal_i2s.h"
#include "ove_backend_common.h"

/*
 * POSIX I2S stub.  On host PC the audio subsystem uses the sim framework instead
 * of I2S — this backend returns NOT_SUPPORTED.
 */

int ove_hal_i2s_open(ove_i2s_t i2s, const struct ove_i2s_cfg *cfg)
{
	(void)i2s;
	(void)cfg;
	return OVE_ERR_NOT_SUPPORTED;
}

void ove_hal_i2s_close(ove_i2s_t i2s)
{
	(void)i2s;
}
int ove_hal_i2s_start(ove_i2s_t i2s)
{
	(void)i2s;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2s_stop(ove_i2s_t i2s)
{
	(void)i2s;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2s_pause(ove_i2s_t i2s)
{
	(void)i2s;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2s_resume(ove_i2s_t i2s)
{
	(void)i2s;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_I2S */
