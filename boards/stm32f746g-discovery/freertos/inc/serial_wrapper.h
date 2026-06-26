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

/* Polled USART1 console for the Linux personality.  The personality drives the console from
 * the svc-exception context, where the IRQ-filled circBuff cannot be used (the svc masks the
 * prio-2 USART1 IRQ, so a blocking read would never see the buffer fill → deadlock).
 * serial_poll_begin() guarantees USART1 is initialized and hands the receiver to the poller
 * (disables the RX IRQ); the rest poll the USART registers directly. */
void serial_poll_begin(void);
int serial_poll_rx_ready(void);
int serial_poll_getc(void);
void serial_poll_putc(char c);

#endif /* INC_SERIAL_WRAPPER_H_ */
