/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef INC_SERIAL_WRAPPER_H_
#define INC_SERIAL_WRAPPER_H_

void serial_init(void);
void serial_write(const unsigned char *data, unsigned int length);
unsigned char serial_getChar(void);
void serial_safe_write(const char *str, unsigned int len);

#endif /* INC_SERIAL_WRAPPER_H_ */
