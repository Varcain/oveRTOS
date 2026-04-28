/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_UART

#include "ove/hal/hal_uart.h"
#include "ove_backend_common.h"

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

/* Forward declaration of the portable ISR push helper */
extern void ove_uart_rx_isr_push(ove_uart_t uart, const void *data, size_t len);

/* RX polling thread — reads from fd and pushes into the portable stream */
static void *posix_uart_rx_thread(void *arg)
{
	ove_uart_t uart = (ove_uart_t)arg;
	uint8_t buf[64];

	while (uart->running) {
		struct pollfd pfd = {
			.fd = uart->fd,
			.events = POLLIN,
		};

		int ret = poll(&pfd, 1, 100); /* 100ms poll timeout */
		if (ret > 0 && (pfd.revents & POLLIN)) {
			ssize_t n = read(uart->fd, buf, sizeof(buf));
			if (n > 0)
				ove_uart_rx_isr_push(uart, buf, (size_t)n);
		}
	}
	return NULL;
}

static speed_t baud_to_speed(uint32_t baudrate)
{
	switch (baudrate) {
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	case 230400:
		return B230400;
#ifdef B460800
	case 460800:
		return B460800;
#endif
#ifdef B921600
	case 921600:
		return B921600;
#endif
	default:
		return B115200;
	}
}

int ove_hal_uart_open(ove_uart_t uart, const struct ove_uart_cfg *cfg)
{
	char path[32];
	struct termios tty;

	snprintf(path, sizeof(path), "/dev/ttyUSB%u", cfg->instance);

	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		/* Try /dev/ttyACM as fallback */
		snprintf(path, sizeof(path), "/dev/ttyACM%u", cfg->instance);
		fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd < 0)
			return OVE_ERR_INVALID_PARAM;
	}

	if (tcgetattr(fd, &tty) != 0) {
		close(fd);
		return OVE_ERR_NOT_SUPPORTED;
	}

	cfmakeraw(&tty);

	speed_t spd = baud_to_speed(cfg->baudrate);
	cfsetispeed(&tty, spd);
	cfsetospeed(&tty, spd);

	/* Data bits */
	tty.c_cflag &= ~CSIZE;
	switch (cfg->data_bits) {
	case 7:
		tty.c_cflag |= CS7;
		break;
	default:
		tty.c_cflag |= CS8;
		break;
	}

	/* Parity */
	if (cfg->parity != OVE_UART_PARITY_NONE) {
		tty.c_cflag |= PARENB;
		if (cfg->parity == OVE_UART_PARITY_ODD)
			tty.c_cflag |= PARODD;
	}

	/* Stop bits */
	if (cfg->stop_bits == OVE_UART_STOP_2)
		tty.c_cflag |= CSTOPB;

	/* Flow control */
	if (cfg->flow_control == OVE_UART_FLOW_RTS_CTS)
		tty.c_cflag |= CRTSCTS;

	tty.c_cflag |= CLOCAL | CREAD;

	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
		close(fd);
		return OVE_ERR_NOT_SUPPORTED;
	}

	uart->fd = fd;
	uart->running = 0;
	return OVE_OK;
}

void ove_hal_uart_close(ove_uart_t uart)
{
	if (uart->running) {
		uart->running = 0;
		pthread_join(uart->rx_thread, NULL);
	}
	if (uart->fd >= 0) {
		close(uart->fd);
		uart->fd = -1;
	}
}

int ove_hal_uart_tx(ove_uart_t uart, const void *data, size_t len, uint32_t timeout_ms,
		    size_t *bytes_written)
{
	(void)timeout_ms;

	ssize_t n = write(uart->fd, data, len);
	if (n < 0)
		return OVE_ERR_BUS_ERROR;

	if (bytes_written != NULL)
		*bytes_written = (size_t)n;
	return OVE_OK;
}

int ove_hal_uart_rx_enable(ove_uart_t uart)
{
	if (uart->fd < 0)
		return OVE_ERR_NOT_SUPPORTED;

	uart->running = 1;
	if (pthread_create(&uart->rx_thread, NULL, posix_uart_rx_thread, uart) != 0) {
		uart->running = 0;
		return OVE_ERR_NO_MEMORY;
	}
	return OVE_OK;
}

int ove_hal_uart_tx_flush(ove_uart_t uart)
{
	if (uart->fd >= 0)
		tcdrain(uart->fd);
	return OVE_OK;
}

#endif /* CONFIG_OVE_UART */
