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

/* IRQ-buffered USART1 console helpers for the Linux personality.
 * serial_poll_begin() guarantees initialization and assigns SVCall an RTOS-API-safe
 * priority; rx_ready/getc inspect the IRQ-filled circular buffer without blocking.
 * putc is a bounded best-effort path reserved for small diagnostics. */
void serial_poll_begin(void);
int serial_poll_rx_ready(void);
int serial_poll_getc(void);
void serial_poll_putc(char c);

#endif /* INC_SERIAL_WRAPPER_H_ */
