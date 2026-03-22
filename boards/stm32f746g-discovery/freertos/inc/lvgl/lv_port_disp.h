/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef _LV_PORT_DISP_H_
#define _LV_PORT_DISP_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize LCD hardware (SDRAM, LTDC, GPIO, clocks)
 *
 * Must be called early in main(), before SD card or other FMC-sensitive init.
 */
void lv_port_disp_hw_init(void);

/**
 * @brief Initialize the LVGL display driver
 *
 * Must be called after lv_init() and lv_port_disp_hw_init().
 */
void lv_port_disp_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _LV_PORT_DISP_H_ */
